// Copyright CustomUnrealMCP. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Tickable.h"
#include "Containers/Queue.h"
#include "Containers/Array.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"

class FSocket;

// Per-client connection handler; runs HandleConnection on its own thread.
class FClientConnectionRunnable;

struct FMCPActiveClient
{
	FRunnableThread* Thread = nullptr;
	TSharedPtr<FClientConnectionRunnable> Runnable;
};

// ─────────────────────────────────────────────────────────────────────────────
// Connection state — drives the toolbar indicator colour
// ─────────────────────────────────────────────────────────────────────────────
enum class EMCPConnectionState : uint8
{
	ServerFailed,   // 🔴 Red    — couldn't bind port (already in use?)
	Waiting,        // 🟡 Amber  — server running, no client connected yet
	Connected,      // 🟢 Green  — Python MCP server is actively connected
};

// ─────────────────────────────────────────────────────────────────────────────
// A command received from the Python MCP server over TCP,
// waiting to be executed on the game thread.
// ─────────────────────────────────────────────────────────────────────────────
struct FMCPPendingCommand
{
	FString Id;
	FString Type;
	TSharedPtr<FJsonObject> Params;

	/** Optional client id (set by connection handler) for rate limiting */
	FString ClientId;

	/** True if the incoming request included "jsonrpc":"2.0" — response will mirror JSON-RPC 2.0 format */
	bool    bJsonRpc      = false;

	bool    bSuccess      = false;
	TSharedPtr<FJsonObject> ResultObject;
	FString ErrorMessage;

	FEvent* CompletionEvent = nullptr;

	/** Machine-readable error code string (e.g. "not_found", "invalid_params"); empty on success */
	FString ErrorCode;

	/** JSON-RPC numeric error code (used when bJsonRpc is true) */
	int32   JsonRpcErrorCode = -32603;  // Internal error default
};

// ─────────────────────────────────────────────────────────────────────────────
// MCP TCP Server
//   • Runs an accept loop on a background FRunnable thread.
//   • Each accepted connection is handled sequentially (read → queue → respond).
//   • FTickableEditorObject::Tick() processes the command queue on the game thread.
// ─────────────────────────────────────────────────────────────────────────────
class UNREALMCP_API FMCPTCPServer
	: public FRunnable
	, public FTickableEditorObject
{
public:
	FMCPTCPServer();
	virtual ~FMCPTCPServer();

	bool Start(int32 InPort = 55557, int32 InCommandTimeoutSeconds = 30, int32 InMaxCommandsPerTick = 16, int32 InMaxRequestLineBytes = 0, bool bInRateLimitEnabled = false);
	void Stop();

	bool  IsRunning()       const { return bRunning.load(); }
	int32 GetPort()         const { return ListenPort; }

	/** Polled by the toolbar widget every frame — safe to call from game thread */
	EMCPConnectionState GetConnectionState() const
	{
		return static_cast<EMCPConnectionState>(ConnectionState.load());
	}

	/** Human-readable status string for the tooltip */
	FString GetStatusString() const;

	// ── FRunnable ─────────────────────────────────────────────────────────────
	virtual bool   Init() override;
	virtual uint32 Run()  override;
	virtual void   Exit() override;

	// ── FTickableEditorObject ─────────────────────────────────────────────────
	virtual void    Tick(float DeltaTime) override;
	virtual bool    IsTickable() const    override { return bRunning.load(); }
	virtual TStatId GetStatId()  const    override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FMCPTCPServer, STATGROUP_Tickables);
	}

private:
	// ── Server state ──────────────────────────────────────────────────────────
	int32             ListenPort    = 55557;
	int32             CommandTimeoutSeconds = 30;
	int32             MaxCommandsPerTick   = 16;
	int32             MaxRequestLineBytes  = 16 * 1024 * 1024;
	bool              bRateLimitEnabled    = false;
	FSocket*          ListenSocket  = nullptr;
	FRunnableThread*  Thread        = nullptr;
	std::atomic<bool> bRunning      { false };
	std::atomic<bool> bShouldStop   { false };

	// Connection state — written by network thread, read by game thread (atomic)
	std::atomic<uint8> ConnectionState
		{ static_cast<uint8>(EMCPConnectionState::Waiting) };

	// Timestamp of last successful client connection (for tooltip)
	std::atomic<int64> LastConnectedTimestamp { 0 };

	// ── Command queue ─────────────────────────────────────────────────────────
	TQueue<TSharedPtr<FMCPPendingCommand>, EQueueMode::Mpsc> CommandQueue;

	// ── Multi-client: active connections and finished queue ───────────────────
	mutable FCriticalSection    ActiveClientsLock;
	TArray<FMCPActiveClient>    ActiveClients;
	TQueue<FClientConnectionRunnable*, EQueueMode::Mpsc> FinishedClients;
	std::atomic<int32>          NumConnectedClients { 0 };

	/** Rate limiting: token bucket per ClientId (optional, see config) */
	struct FTokenBucketState { double Tokens = 100.0; double LastReplenishTime = 0.0; };
	mutable FCriticalSection    RateLimitLock;
	TMap<FString, FTokenBucketState> ClientTokens;
	bool TryConsumeTokens(const FString& ClientId, int32 Cost, FString* OutError = nullptr);
	void ReplenishTokens(FTokenBucketState& State, double Now);

	void OnClientConnected();
	void OnClientDisconnected();
	void EnqueueFinishedClient(FClientConnectionRunnable* Runnable);
	void ProcessFinishedClients();

	// ── Helpers ───────────────────────────────────────────────────────────────
	void HandleConnection(FSocket* ClientSocket, const FString& ClientId);

	friend class FClientConnectionRunnable;
	bool ReadLine(FSocket* Socket, FString& OutLine, TArray<uint8>& InOutBuf, int32& InOutBufPos);
	void SendString(FSocket* Socket, const FString& Text);

	TSharedPtr<FMCPPendingCommand> ParseCommand(const FString& RawJson);
	FString SerialiseResponse(const TSharedPtr<FMCPPendingCommand>& Cmd);

	void DispatchCommand(TSharedPtr<FMCPPendingCommand>& Cmd);

	void Cmd_Ping             (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_ExecutePython    (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_ConsoleCommand   (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetProjectInfo   (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetAllActors     (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SpawnActor       (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_DeleteActor      (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_DeleteActors    (TSharedPtr<FMCPPendingCommand>& Cmd);  // bulk delete by filter
	void Cmd_GetActorTransform(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetActorTransform(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_OpenLevel        (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetAssets        (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetActorProperty (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetComponentProperty(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetActorComponents(TSharedPtr<FMCPPendingCommand>& Cmd);
	// ── Viewport / level editor ───────────────────────────────────────────────
	void Cmd_FocusViewport       (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_TakeScreenshot      (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetViewportTransform(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetViewportFOV     (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetWorldContexts    (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetSelectedActors   (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetSelectedActors   (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetCurrentLevel     (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetLoadedLevels    (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_LoadStreamingLevel  (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_UnloadStreamingLevel(TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── Outliner / folders ────────────────────────────────────────────────────
	void Cmd_SetActorFolder           (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetSelectedActorsFolder  (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_ListActorFolders         (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_CreateActorFolder       (TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── Blueprint graph commands ───────────────────────────────────────────────
	void Cmd_AddBlueprintComponent   (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_RemoveBlueprintComponent(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_CreateBlueprint      (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_FindBlueprintNodes   (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintEventNode(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintFuncNode (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_ConnectBlueprintNodes(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_CompileBlueprint     (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintBranchNode  (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintSequenceNode(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintSwitchNode  (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetStatus               (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintTimelineNode(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintForEachNode (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintSwitchStringNode(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintSwitchEnumNode  (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintGateNode        (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintMultiGateNode   (TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── Python file execution ─────────────────────────────────────────────────
	void Cmd_ExecutePythonFile       (TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── Actor read / duplicate ────────────────────────────────────────────────
	void Cmd_GetActorProperty        (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetComponentProperty    (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetAllProperties        (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_DuplicateActor          (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_PlaceActorFromAsset     (TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── Asset management ──────────────────────────────────────────────────────
	void Cmd_SaveAsset               (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SaveLevel               (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SaveAll                 (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_DeleteAsset             (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_DuplicateAsset          (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_RenameAsset             (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_CreateMaterial          (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetMaterialExpressions  (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetAssetFullMetadata    (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetSelectedAssets       (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetSelectedAssets      (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_CreateAsset             (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetContentSubpaths      (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_CreateContentFolder     (TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── Blueprint graph read ──────────────────────────────────────────────────
	void Cmd_GetBlueprintGraphs      (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_CreateBlueprintGraph    (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetNodeInfo             (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_DeleteBlueprintNode     (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_DisconnectBlueprintPins (TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── Blueprint variable management ────────────────────────────────────────
	void Cmd_GetBlueprintVariables      (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintVariable       (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetBlueprintVariableDefault(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlueprintVariableNode   (TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── New commands ──────────────────────────────────────────────────────────
	void Cmd_SetNodePosition  (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetActorBounds   (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetPinDefaultValue(TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetUnsavedAssets  (TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── Material editing ─────────────────────────────────────────────────────
	void Cmd_ConnectMaterialExpressions      (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_ConnectMaterialProperty         (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddMaterialExpression           (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetMaterialExpressionProperty   (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetMaterialExpressionPins       (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_DeleteMaterialExpression        (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_RecompileMaterial               (TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── UMG (Widget Blueprint tree) ───────────────────────────────────────────
	void Cmd_AddUmgWidget              (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_RemoveUmgWidget           (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_GetUmgTree                (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetUmgSlotContent         (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_CreateWidgetBlueprint     (TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── AI (Behavior Tree, Blackboard, AI Controller, Perception, Navigation) ─
	void Cmd_CreateBehaviorTree    (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBlackboardKey      (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_RunBehaviorTree       (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_ConfigureAIPerception (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_ConfigureAIHearing    (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBTCompositeNode    (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBTDecoratorNode    (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_AddBTServiceNode      (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_RebuildNavigation     (TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── Viewport enumeration ──────────────────────────────────────────────────
	void Cmd_ListViewports         (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_SetBlueprintCDOProperty(TSharedPtr<FMCPPendingCommand>& Cmd);

	// ── MCP protocol (JSON-RPC 2.0 compatibility) ────────────────────────────
	void Cmd_Initialize            (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_ToolsList             (TSharedPtr<FMCPPendingCommand>& Cmd);
	void Cmd_ToolsCall             (TSharedPtr<FMCPPendingCommand>& Cmd);

	/** Build the tool definitions array for tools/list response */
	TArray<TSharedPtr<FJsonValue>> BuildToolDefinitions() const;

	/** Get world from params: "use_pie" (use first PIE world) or "world_context_index" (int); else editor world. */
	static class UWorld* GetWorldFromParams(const TSharedPtr<FJsonObject>& Params);

	AActor* FindActorByLabel(class UWorld* World, const FString& Label);
	/** Resolve actor by editor path (e.g. PersistentLevel.StaticMeshActor_0) or full path. */
	AActor* FindActorByPath(class UWorld* World, const FString& Path);
	/** Resolve actor from params: tries "actor_path" then "actor_label". Uses world from params (use_pie/world_context_index). */
	AActor* ResolveActorFromParams(const TSharedPtr<FJsonObject>& Params, FString* OutError = nullptr);

	static UBlueprint* FindBlueprintByName(const FString& Name);
	static UClass*     FindClassByName    (const FString& Name);

	static void SetSuccess(TSharedPtr<FMCPPendingCommand>& Cmd, TSharedPtr<FJsonObject> Result);
	static void SetError  (TSharedPtr<FMCPPendingCommand>& Cmd, const FString& Message, const FString& Code = TEXT("error"));

	// ── Property helpers — write ──────────────────────────────────────────────
	/** Convert JSON value to string representation for ImportText */
	static FString JsonValueToString(TSharedPtr<FJsonValue> ValueJson);

	/** Set a nested property using dot notation path (e.g., "Settings.AutoExposureBias") */
	static bool SetNestedProperty(UObject* TargetObject, const FString& PropertyPath,
		TSharedPtr<FJsonValue> ValueJson, FString& OutErrorMessage);

	/** Set a property value with type-aware conversion */
	static bool SetPropertyValue(FProperty* Prop, void* Container, TSharedPtr<FJsonValue> ValueJson,
		FString& OutErrorMessage);

	// ── Property helpers — read ───────────────────────────────────────────────
	/** Read a single property and return it as a JSON value (Container = UObject/struct instance) */
	static TSharedPtr<FJsonValue> GetPropertyValue(FProperty* Prop, const void* Container);

	/** Read a property from a direct value pointer (no container offset) */
	static TSharedPtr<FJsonValue> GetPropertyValueDirect(FProperty* Prop, const void* ValuePtr);

	/** Read a property from an UObject using dot-notation path */
	static TSharedPtr<FJsonValue> GetNestedProperty(UObject* Obj, const FString& DotPath);

	/** Find an EdGraph inside a Blueprint by name (searches all graph arrays) */
	static UEdGraph* FindGraphByName(UBlueprint* BP, const FString& GraphName);

	// ── Internal state setter (call from network thread only) ─────────────────
	void SetConnectionState(EMCPConnectionState NewState)
	{
		ConnectionState.store(static_cast<uint8>(NewState));
	}
};
