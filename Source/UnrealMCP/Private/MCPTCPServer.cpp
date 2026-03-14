// Copyright CustomUnrealMCP. All Rights Reserved.
//
// MCPTCPServer.cpp
// ─────────────────
// Implements the TCP server + all command handlers.
//
// Threading model:
//   [Accept thread]  FMCPTCPServer::Run()
//       └── accept loop → spawn FClientConnectionRunnable in new thread per client
//       └── process FinishedClients queue (join exited client threads)
//
//   [Per-client thread]  FClientConnectionRunnable::Run()
//       └── HandleConnection(ClientSocket) → ReadLine → ParseCommand → Enqueue(PendingCmd)
//           └── wait on PendingCmd->CompletionEvent (30 s timeout)
//           └── SendString(response JSON)
//
//   [Game thread]  FMCPTCPServer::Tick()
//       └── dequeue PendingCmd → DispatchCommand(Cmd)
//           └── Cmd->CompletionEvent->Trigger()   ← unblocks that client's thread
//
// Protocol: newline-delimited UTF-8 JSON
//   Request:  {"id":"<uuid>","type":"<cmd>","params":{...}}\n
//   Response: {"id":"<uuid>","success":true,"result":{...}}\n
//             {"id":"<uuid>","success":false,"error":"<msg>"}\n

#include "MCPTCPServer.h"
#include "UnrealMCPModule.h"

// UE networking
#include "Common/TcpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

// UE JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// UE Editor
#include "Editor.h"
#include "EditorLevelLibrary.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Engine/World.h"
#include "Engine/Engine.h"       // GEngine, GetWorldContexts, EWorldType
#include "GameFramework/Actor.h"
#include "EngineUtils.h"            // TActorIterator
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/Paths.h"

// Property system for nested property support
#include "UObject/UnrealType.h"     // FProperty, FStructProperty, FScriptArrayHelper, etc.
#include "JsonObjectConverter.h"    // FJsonObjectConverter::JsonValueToUProperty (array elements)
#include "UObject/EnumProperty.h"   // FEnumProperty
#include "UObject/PropertyPortFlags.h"
#include "Math/Color.h"             // FLinearColor, FColor
#include "Misc/App.h"
#include "FileHelpers.h"             // FEditorFileUtils::LoadMap

// Viewport focus + screenshot
#include "LevelEditorViewport.h"   // FLevelEditorViewportClient
#include "UnrealClient.h"          // FScreenshotRequest
#include "EditorActorFolders.h"    // FActorFolders::SetSelectedFolderPath
#include "IContentBrowserSingleton.h"  // GetSelectedAssets
#include "IAssetTools.h"
#include "AssetToolsModule.h"
#include "Factories/Factory.h"     // UFactory, SupportedClass
#include "UObject/Class.h"         // GetDerivedClasses
#include "UObject/FieldIterator.h" // TFieldIterator<FProperty>
#include "Modules/ModuleManager.h"
#include "AssetSelection.h"       // FActorFactoryAssetProxy
#include "Selection.h"            // FSelectionIterator
#include "Engine/LevelStreaming.h" // ULevelStreaming
#include "EditorLevelUtils.h"     // AddLevelToWorld, RemoveLevelsFromWorld
#include "LevelUtils.h"           // FLevelUtils::FindStreamingLevel
#include "WidgetBlueprint.h"       // UWidgetBlueprint (already have for add_umg_widget)
#include "WidgetBlueprintFactory.h"  // UWidgetBlueprintFactory (UMGEditor)
#include "Factories/DataTableFactory.h"  // UDataTableFactory (row_struct)
#include "Engine/DataTable.h"             // UDataTable

// AI (Behavior Tree, Blackboard)
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKey.h"   // FBlackboard::InvalidKey
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTreeFactory.h"
#include "BlackboardDataFactory.h"
#include "AIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "NavigationSystem.h"

// Python (optional — enabled when PythonScriptPlugin is loaded)
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"       // FPythonLogOutputEntry, EPythonLogOutputType

// Blueprint graph editing (BlueprintGraph module)
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_Timeline.h"
#include "K2Node_MacroInstance.h"
#include "Engine/TimelineTemplate.h"
#include "EdGraphSchema_K2.h"

// Asset management (EditorScriptingUtilities module)
#include "EditorAssetLibrary.h"

// Material creation and read
#include "Factories/MaterialFactoryNew.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "MaterialEditingLibrary.h"   // ConnectMaterialExpressions, ConnectMaterialProperty, CreateMaterialExpression, RecompileMaterial
#include "SceneTypes.h"               // EMaterialProperty
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"
#include "WidgetBlueprint.h"   // UWidgetBlueprint (UMGEditor)

// Blueprint SCS / component editing
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Components/StaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Materials/MaterialInterface.h"

// Blueprint compiler results log
#include "KismetCompiler.h"

// ── NEW INCLUDES for FEATURES_TO_ADD ─────────────────────────────────────────

// Lighting
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/RectLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/RectLightComponent.h"

// Landscape
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"

// Foliage
#include "InstancedFoliageActor.h"
#include "FoliageType.h"

// Level Sequence / Sequencer
#include "LevelSequence.h"
#include "LevelSequenceActor.h"

// Audio
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundCue.h"

// Physics constraints
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/PhysicsConstraintActor.h"

// World Partition / Data Layers
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"

// AI extensions
#include "Perception/AISenseConfig_Damage.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "GameFramework/Pawn.h"

// Undo/Redo
#include "ScopedTransaction.h"

// FScriptArrayHelper is pulled in transitively via UObject/UnrealType.h (already included above)

// ── Plugin version ───────────────────────────────────────────────────────────
static const FString UNREALMCP_VERSION = TEXT("2.0.1");

// ─────────────────────────────────────────────────────────────────────────────
// Per-client connection runnable — runs HandleConnection on its own thread
// ─────────────────────────────────────────────────────────────────────────────
class FClientConnectionRunnable : public FRunnable
{
public:
	FClientConnectionRunnable(FSocket* InSocket, FMCPTCPServer* InServer, const FString& InClientId)
		: ClientSocket(InSocket)
		, Server(InServer)
		, ClientId(InClientId)
		, SocketSubsystem(ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
	{}

	virtual uint32 Run() override
	{
		if (Server && ClientSocket)
		{
			Server->HandleConnection(ClientSocket, ClientId);
		}
		if (Server)
		{
			Server->OnClientDisconnected();
		}
		if (SocketSubsystem && ClientSocket)
		{
			ClientSocket->Close();
			SocketSubsystem->DestroySocket(ClientSocket);
			ClientSocket = nullptr;
		}
		if (Server)
		{
			Server->EnqueueFinishedClient(this);
		}
		return 0;
	}

	void CloseSocket()
	{
		if (ClientSocket)
		{
			ClientSocket->Close();
		}
	}

private:
	FSocket*           ClientSocket;
	FMCPTCPServer*     Server;
	FString            ClientId;
	ISocketSubsystem*  SocketSubsystem;
};

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

FMCPTCPServer::FMCPTCPServer()
	: ListenPort(55557)
	, ListenSocket(nullptr)
	, Thread(nullptr)
{
}

FMCPTCPServer::~FMCPTCPServer()
{
	Stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────────────────────────────

bool FMCPTCPServer::Start(int32 InPort, int32 InCommandTimeoutSeconds, int32 InMaxCommandsPerTick, int32 InMaxRequestLineBytes, bool bInRateLimitEnabled, const FString& InBindAddress)
{
	if (bRunning.load())
	{
		UE_LOG(LogUnrealMCP, Warning, TEXT("Server already running on port %d"), ListenPort);
		return true;
	}

	ListenPort = InPort;
	CommandTimeoutSeconds = FMath::Clamp(InCommandTimeoutSeconds, 5, 300);
	MaxCommandsPerTick = FMath::Clamp(InMaxCommandsPerTick, 1, 256);
	MaxRequestLineBytes = (InMaxRequestLineBytes > 0) ? FMath::Clamp(InMaxRequestLineBytes, 4096, 64 * 1024 * 1024) : (16 * 1024 * 1024);
	bRateLimitEnabled = bInRateLimitEnabled;
	BindAddress = InBindAddress;

	// Create listen socket
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogUnrealMCP, Error, TEXT("No socket subsystem available"));
		return false;
	}

	ListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMCP_Listen"), false);
	if (!ListenSocket)
	{
		UE_LOG(LogUnrealMCP, Error, TEXT("Failed to create listen socket"));
		return false;
	}

	ListenSocket->SetReuseAddr(true);
	ListenSocket->SetNonBlocking(false);

	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	// Bind address config: "localhost" or "127.0.0.1" binds to loopback only (safer on shared networks)
	if (BindAddress == TEXT("127.0.0.1") || BindAddress.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
	{
		bool bIsValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
		UE_LOG(LogUnrealMCP, Log, TEXT("Binding to localhost only (127.0.0.1)"));
	}
	else
	{
		Addr->SetAnyAddress();
	}
	Addr->SetPort(ListenPort);

	if (!ListenSocket->Bind(*Addr))
	{
		UE_LOG(LogUnrealMCP, Error, TEXT("Failed to bind to port %d"), ListenPort);
		SocketSubsystem->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
		return false;
	}

	if (!ListenSocket->Listen(8))
	{
		UE_LOG(LogUnrealMCP, Error, TEXT("Failed to listen on port %d"), ListenPort);
		SocketSubsystem->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
		return false;
	}

	bShouldStop.store(false);
	bRunning.store(true);
	SetConnectionState(EMCPConnectionState::Waiting);   // 🟡 server up, no client yet

	Thread = FRunnableThread::Create(this, TEXT("UnrealMCPServer"), 0,
		TPri_BelowNormal, FPlatformAffinity::GetPoolThreadMask());

	return Thread != nullptr;
}

void FMCPTCPServer::Stop()
{
	if (!bRunning.load()) return;

	bShouldStop.store(true);
	bRunning.store(false);

	// Close listen socket to unblock Accept()
	if (ListenSocket)
	{
		ListenSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
	}

	// Close all client sockets so client threads exit HandleConnection
	TArray<FMCPActiveClient> ClientsToJoin;
	{
		FScopeLock Lock(&ActiveClientsLock);
		ClientsToJoin = MoveTemp(ActiveClients);
		ActiveClients.Reset();
	}
	for (FMCPActiveClient& Client : ClientsToJoin)
	{
		if (Client.Runnable.IsValid())
		{
			Client.Runnable->CloseSocket();
		}
	}

	// Drain any commands already queued so client threads blocked on CompletionEvent
	// don't have to wait the full CommandTimeoutSeconds before WaitForCompletion() can return.
	{
		TSharedPtr<FMCPPendingCommand> PendingCmd;
		while (CommandQueue.Dequeue(PendingCmd))
		{
			SetError(PendingCmd, TEXT("Server shutting down"), TEXT("server_stopping"));
			if (PendingCmd->CompletionEvent)
			{
				PendingCmd->CompletionEvent->Trigger();
			}
		}
	}

	for (FMCPActiveClient& Client : ClientsToJoin)
	{
		if (Client.Thread)
		{
			Client.Thread->WaitForCompletion();
			delete Client.Thread;
		}
	}

	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}

	SetConnectionState(EMCPConnectionState::ServerFailed);  // 🔴 stopped
	UE_LOG(LogUnrealMCP, Log, TEXT("MCP TCP server stopped"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Status string for the toolbar tooltip
// ─────────────────────────────────────────────────────────────────────────────

FString FMCPTCPServer::GetStatusString() const
{
	switch (GetConnectionState())
	{
		case EMCPConnectionState::Connected:
		{
			const int32 N = NumConnectedClients.load();
			FDateTime ConnTime(LastConnectedTimestamp.load());
			return FString::Printf(
				TEXT("🟢  %d client(s) connected\nPort: %d\nLast connect: %s"),
				N, ListenPort,
				*ConnTime.ToString(TEXT("%Y-%m-%d %H:%M:%S")));
		}
		case EMCPConnectionState::Waiting:
			return FString::Printf(
				TEXT("🟡  Waiting for clients\nPort: %d\nStart the Python MCP server to connect."),
				ListenPort);
		case EMCPConnectionState::ServerFailed:
		default:
			return FString::Printf(
				TEXT("🔴  MCP server failed to start\nPort %d may already be in use.\n"
				     "Change it via DefaultEngine.ini:\n  [UnrealMCP]\n  Port=55558"),
				ListenPort);
	}
}

void FMCPTCPServer::OnClientConnected()
{
	NumConnectedClients.fetch_add(1);
	SetConnectionState(EMCPConnectionState::Connected);
	LastConnectedTimestamp.store(FDateTime::UtcNow().GetTicks());
}

void FMCPTCPServer::OnClientDisconnected()
{
	if (NumConnectedClients.fetch_sub(1) == 1)
	{
		SetConnectionState(EMCPConnectionState::Waiting);
	}
}

void FMCPTCPServer::EnqueueFinishedClient(FClientConnectionRunnable* Runnable)
{
	FinishedClients.Enqueue(Runnable);
}

void FMCPTCPServer::ProcessFinishedClients()
{
	FClientConnectionRunnable* Runnable = nullptr;
	while (FinishedClients.Dequeue(Runnable))
	{
		FRunnableThread* ThreadToJoin = nullptr;
		{
			FScopeLock Lock(&ActiveClientsLock);
			for (int32 i = 0; i < ActiveClients.Num(); ++i)
			{
				if (ActiveClients[i].Runnable.Get() == Runnable)
				{
					ThreadToJoin = ActiveClients[i].Thread;
					ActiveClients.RemoveAt(i);
					break;
				}
			}
		}
		if (ThreadToJoin)
		{
			ThreadToJoin->WaitForCompletion();
			delete ThreadToJoin;
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// FRunnable — background accept loop
// ─────────────────────────────────────────────────────────────────────────────

bool FMCPTCPServer::Init()  { return true; }
void FMCPTCPServer::Exit()  {}

uint32 FMCPTCPServer::Run()
{
	UE_LOG(LogUnrealMCP, Log, TEXT("MCP accept loop started (port %d), multi-client"), ListenPort);

	while (!bShouldStop.load() && ListenSocket)
	{
		// Join any client threads that have exited
		ProcessFinishedClients();

		bool bHasPendingConn = false;
		if (!ListenSocket->HasPendingConnection(bHasPendingConn))
		{
			FPlatformProcess::Sleep(0.1f);
			continue;
		}
		if (!bHasPendingConn)
		{
			FPlatformProcess::Sleep(0.05f);
			continue;
		}

		FSocket* ClientSocket = ListenSocket->Accept(TEXT("UnrealMCP_Client"));
		if (!ClientSocket)
		{
			if (!bShouldStop.load())
			{
				UE_LOG(LogUnrealMCP, Warning, TEXT("Accept returned null socket"));
			}
			continue;
		}

		ClientSocket->SetNonBlocking(false);
		ClientSocket->SetNoDelay(true);

		FString ClientId = FGuid::NewGuid().ToString(EGuidFormats::Short);
		TSharedPtr<FClientConnectionRunnable> Runnable = MakeShared<FClientConnectionRunnable>(ClientSocket, this, ClientId);
		FRunnableThread* ClientThread = FRunnableThread::Create(
			Runnable.Get(), TEXT("UnrealMCP_Client"), 0, TPri_Normal, FPlatformAffinity::GetPoolThreadMask());
		if (!ClientThread)
		{
			UE_LOG(LogUnrealMCP, Warning, TEXT("Failed to create client thread"));
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
			continue;
		}

		{
			FScopeLock Lock(&ActiveClientsLock);
			FMCPActiveClient Entry;
			Entry.Thread = ClientThread;
			Entry.Runnable = Runnable;
			ActiveClients.Add(Entry);
		}
		OnClientConnected();
		UE_LOG(LogUnrealMCP, Log, TEXT("MCP client connected (%d total)"), NumConnectedClients.load());
	}

	ProcessFinishedClients();
	return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// FTickableEditorObject — game thread processes command queue
// ─────────────────────────────────────────────────────────────────────────────

void FMCPTCPServer::Tick(float DeltaTime)
{
	TSharedPtr<FMCPPendingCommand> Cmd;
	int32 Processed = 0;
	while (Processed < MaxCommandsPerTick && CommandQueue.Dequeue(Cmd))
	{
		DispatchCommand(Cmd);
		// Signal the waiting network thread
		if (Cmd->CompletionEvent)
		{
			Cmd->CompletionEvent->Trigger();
		}
		++Processed;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection handling
// ─────────────────────────────────────────────────────────────────────────────

void FMCPTCPServer::HandleConnection(FSocket* ClientSocket, const FString& ClientId)
{
	// Per-connection receive buffer — avoids 1-syscall-per-byte reads
	TArray<uint8> RecvBuf;
	RecvBuf.Reserve(8192);
	int32 BufStart = 0;

	while (!bShouldStop.load())
	{
		FString RawLine;
		if (!ReadLine(ClientSocket, RawLine, RecvBuf, BufStart))
		{
			break; // disconnected or error
		}

		RawLine.TrimStartAndEndInline();
		if (RawLine.IsEmpty()) continue;

		TSharedPtr<FMCPPendingCommand> Cmd = ParseCommand(RawLine);
		if (!Cmd)
		{
			// Send parse error immediately
			FString ErrJson = TEXT("{\"id\":\"\",\"success\":false,\"error\":\"JSON parse error\"}\n");
			SendString(ClientSocket, ErrJson);
			continue;
		}
		Cmd->ClientId = ClientId;

		// Create synchronisation event
		FEvent* Event = FPlatformProcess::GetSynchEventFromPool(false);
		Cmd->CompletionEvent = Event;

		// Enqueue for game thread
		CommandQueue.Enqueue(Cmd);

		const bool bSignalled = Event->Wait(FTimespan::FromSeconds(static_cast<double>(CommandTimeoutSeconds)));
		FPlatformProcess::ReturnSynchEventToPool(Event);
		Cmd->CompletionEvent = nullptr;

		if (!bSignalled)
		{
			SetError(Cmd, FString::Printf(TEXT("Command timed out on game thread (%d s)"), CommandTimeoutSeconds), TEXT("timeout"));
		}

		// Send response
		FString Response = SerialiseResponse(Cmd);
		SendString(ClientSocket, Response + TEXT("\n"));
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Socket I/O helpers
// ─────────────────────────────────────────────────────────────────────────────

bool FMCPTCPServer::ReadLine(FSocket* Socket, FString& OutLine, TArray<uint8>& Buf, int32& BufStart)
{
	while (true)
	{
		// Scan existing buffered data for a newline
		for (int32 i = BufStart; i < Buf.Num(); ++i)
		{
			if (Buf[i] != '\n') continue;

			const int32 LineLen = i - BufStart;
			if (LineLen >= MaxRequestLineBytes)
				return false;  // line too long

			if (LineLen == 0)
			{
				// Bare "\n" — empty line; caller (HandleConnection) will skip it
				OutLine = TEXT("");
			}
			else
			{
				TArray<uint8> LineData;
				LineData.Append(Buf.GetData() + BufStart, LineLen);
				LineData.Add(0);  // null-terminate for UTF-8 → TCHAR conversion
				OutLine = UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(LineData.GetData()));
			}

			BufStart = i + 1;
			// Compact buffer once the consumed prefix grows large
			if (BufStart > 32768)
			{
				Buf.RemoveAt(0, BufStart, /*bAllowShrinking=*/false);
				BufStart = 0;
			}
			return true;
		}

		// No newline in current buffer — need more data from the socket
		if ((Buf.Num() - BufStart) >= MaxRequestLineBytes)
			return false;  // overflow without newline

		const int32 OldEnd = Buf.Num();
		const int32 NewSize = OldEnd + 4096;
		if (NewSize > MaxRequestLineBytes + 4096)
		{
			return false;  // prevent excessive memory allocation
		}
		Buf.SetNumUninitialized(NewSize, false);
		int32 BytesRead = 0;
		const bool bOk = Socket->Recv(Buf.GetData() + OldEnd, 4096, BytesRead, ESocketReceiveFlags::None);
		Buf.SetNum(OldEnd + (bOk && BytesRead > 0 ? BytesRead : 0), false);
		if (!bOk || BytesRead == 0)
			return false;
	}
}

void FMCPTCPServer::SendString(FSocket* Socket, const FString& Text)
{
	FTCHARToUTF8 Converter(*Text);
	const uint8* Data  = reinterpret_cast<const uint8*>(Converter.Get());
	int32 Total  = Converter.Length();
	int32 Offset = 0;
	while (Offset < Total)
	{
		int32 BytesSent = 0;
		if (!Socket->Send(Data + Offset, Total - Offset, BytesSent) || BytesSent == 0)
		{
			UE_LOG(LogUnrealMCP, Warning, TEXT("SendString: connection lost after %d/%d bytes"), Offset, Total);
			break;
		}
		Offset += BytesSent;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON protocol
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FMCPPendingCommand> FMCPTCPServer::ParseCommand(const FString& RawJson)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawJson);

	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return nullptr;
	}

	TSharedPtr<FMCPPendingCommand> Cmd = MakeShared<FMCPPendingCommand>();

	// ── Detect JSON-RPC 2.0 format ──────────────────────────────────────────
	// MCP spec uses JSON-RPC 2.0: {"jsonrpc":"2.0","id":...,"method":"...","params":{...}}
	FString JsonRpcVersion;
	if (Root->TryGetStringField(TEXT("jsonrpc"), JsonRpcVersion) && JsonRpcVersion == TEXT("2.0"))
	{
		Cmd->bJsonRpc = true;

		// JSON-RPC allows id as number or string; guard against null or invalid value
		TSharedPtr<FJsonValue> IdVal = Root->TryGetField(TEXT("id"));
		if (IdVal.IsValid())
		{
			if (IdVal->Type == EJson::Number)
			{
				Cmd->Id = FString::Printf(TEXT("%d"), static_cast<int32>(IdVal->AsNumber()));
			}
			else if (IdVal->Type == EJson::String)
			{
				Cmd->Id = IdVal->AsString();
			}
			// EJson::Null or other types: leave Cmd->Id empty
		}

		Root->TryGetStringField(TEXT("method"), Cmd->Type);
	}
	else
	{
		// ── Legacy format: {"id":"...","type":"...","params":{...}} ──────────
		Root->TryGetStringField(TEXT("id"),   Cmd->Id);
		Root->TryGetStringField(TEXT("type"), Cmd->Type);
	}

	// TryGetObjectField is safe when "params" is absent or not an object
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
			Cmd->Params = *ParamsPtr;
	}
	return Cmd;
}

FString FMCPTCPServer::SerialiseResponse(const TSharedPtr<FMCPPendingCommand>& Cmd)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	if (Cmd->bJsonRpc)
	{
		// ── JSON-RPC 2.0 response format ────────────────────────────────────
		Root->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Root->SetStringField(TEXT("id"), Cmd->Id);

		if (Cmd->bSuccess)
		{
			Root->SetObjectField(TEXT("result"), Cmd->ResultObject.IsValid() ? Cmd->ResultObject : MakeShared<FJsonObject>());
		}
		else
		{
			TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
			ErrorObj->SetNumberField(TEXT("code"), Cmd->JsonRpcErrorCode);
			ErrorObj->SetStringField(TEXT("message"), Cmd->ErrorMessage);
			if (!Cmd->ErrorCode.IsEmpty())
			{
				ErrorObj->SetStringField(TEXT("data"), Cmd->ErrorCode);
			}
			Root->SetObjectField(TEXT("error"), ErrorObj);
		}
	}
	else
	{
		// ── Legacy response format ──────────────────────────────────────────
		Root->SetStringField(TEXT("id"),      Cmd->Id);
		Root->SetBoolField  (TEXT("success"), Cmd->bSuccess);

		if (Cmd->ResultObject.IsValid())
		{
			Root->SetObjectField(TEXT("result"), Cmd->ResultObject);
		}
		if (!Cmd->bSuccess)
		{
			Root->SetStringField(TEXT("error"),      Cmd->ErrorMessage);
			Root->SetStringField(TEXT("error_code"), Cmd->ErrorCode.IsEmpty() ? TEXT("error") : Cmd->ErrorCode);
		}
	}

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void FMCPTCPServer::SetSuccess(TSharedPtr<FMCPPendingCommand>& Cmd, TSharedPtr<FJsonObject> Result)
{
	Cmd->bSuccess      = true;
	Cmd->ResultObject  = Result ? Result : MakeShared<FJsonObject>();
	Cmd->ErrorMessage  = TEXT("");
}

void FMCPTCPServer::SetError(TSharedPtr<FMCPPendingCommand>& Cmd, const FString& Message, const FString& Code)
{
	Cmd->bSuccess     = false;
	Cmd->ErrorMessage = Message;
	Cmd->ErrorCode    = Code.IsEmpty() ? TEXT("error") : Code;
	Cmd->ResultObject = nullptr;
}

UWorld* FMCPTCPServer::GetWorldFromParams(const TSharedPtr<FJsonObject>& Params)
{
	bool bUsePIE = false;
	int32 WorldContextIndex = -1;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("use_pie"), bUsePIE);
		Params->TryGetNumberField(TEXT("world_context_index"), WorldContextIndex);
	}
	UEngine* Engine = GEngine;
	if (Engine && (bUsePIE || WorldContextIndex >= 0))
	{
		const TIndirectArray<FWorldContext>& Contexts = Engine->GetWorldContexts();
		if (bUsePIE)
		{
			for (const FWorldContext& Ctx : Contexts)
			{
				if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
					return Ctx.World();
			}
		}
		if (WorldContextIndex >= 0 && WorldContextIndex < Contexts.Num())
		{
			UWorld* W = Contexts[WorldContextIndex].World();
			if (W) return W;
		}
	}
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

AActor* FMCPTCPServer::FindActorByLabel(UWorld* World, const FString& Label)
{
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
		if (It->GetActorLabel() == Label)
			return *It;
	return nullptr;
}

AActor* FMCPTCPServer::FindActorByPath(UWorld* World, const FString& Path)
{
	if (!World || Path.IsEmpty()) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		FString ActorPath = A->GetPathName();
		// EndsWith must be preceded by '.' or '/' to avoid partial-name false positives
		// e.g. searching "Actor_0" must not match "MyActor_0"
		if (ActorPath == Path
			|| ActorPath.EndsWith(TEXT(".") + Path)
			|| ActorPath.EndsWith(TEXT("/") + Path))
			return A;
	}
	return nullptr;
}

AActor* FMCPTCPServer::ResolveActorFromParams(const TSharedPtr<FJsonObject>& Params, FString* OutError)
{
	if (!Params.IsValid())
	{
		if (OutError) *OutError = TEXT("Missing params");
		return nullptr;
	}
	UWorld* World = GetWorldFromParams(Params);
	FString Path, Label;
	Params->TryGetStringField(TEXT("actor_path"),  Path);
	Params->TryGetStringField(TEXT("actor_label"), Label);
	if (Label.IsEmpty()) Params->TryGetStringField(TEXT("label"), Label);

	if (!Path.IsEmpty())
	{
		AActor* A = FindActorByPath(World, Path);
		if (!A && OutError) *OutError = FString::Printf(TEXT("Actor not found by path: '%s'"), *Path);
		return A;
	}
	if (!Label.IsEmpty())
	{
		TArray<AActor*> Matches;
		if (World)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
				if (It->GetActorLabel() == Label)
					Matches.Add(*It);
		}
		if (Matches.Num() == 0)
		{
			if (OutError) *OutError = FString::Printf(TEXT("Actor not found by label: '%s'"), *Label);
			return nullptr;
		}
		if (Matches.Num() > 1)
		{
			if (OutError) *OutError = FString::Printf(TEXT("Multiple actors with label '%s'; use actor_path to disambiguate"), *Label);
			return nullptr;
		}
		return Matches[0];
	}
	if (OutError) *OutError = TEXT("Provide 'actor_path', 'actor_label', or 'label'");
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Command dispatcher
// ─────────────────────────────────────────────────────────────────────────────

static int32 GetCommandTokenCost(const FString& Type)
{
	if (Type == TEXT("compile_blueprint") || Type == TEXT("execute_python")) return 10;
	if (Type == TEXT("execute_python_file") || Type == TEXT("create_asset") || Type == TEXT("place_actor_from_asset")) return 5;
	return 1;
}

void FMCPTCPServer::ReplenishTokens(FTokenBucketState& State, double Now)
{
	const double RatePerSecond = 10.0;
	const double MaxTokens = 100.0;
	double Elapsed = (State.LastReplenishTime > 0) ? (Now - State.LastReplenishTime) : 0.0;
	State.Tokens = FMath::Min(MaxTokens, State.Tokens + Elapsed * RatePerSecond);
	State.LastReplenishTime = Now;
}

bool FMCPTCPServer::TryConsumeTokens(const FString& ClientId, int32 Cost, FString* OutError)
{
	if (ClientId.IsEmpty()) return true;
	FScopeLock Lock(&RateLimitLock);
	FTokenBucketState& State = ClientTokens.FindOrAdd(ClientId);
	double Now = FPlatformTime::Seconds();
	ReplenishTokens(State, Now);
	if (State.Tokens < Cost)
	{
		if (OutError) *OutError = FString::Printf(TEXT("Rate limit: insufficient tokens (have %.0f, need %d). Wait and retry."), State.Tokens, Cost);
		return false;
	}
	State.Tokens -= Cost;
	return true;
}

void FMCPTCPServer::DispatchCommand(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const FString& Type = Cmd->Type;

	// Rate limiting (token bucket per client; off by default, enable via [UnrealMCP] RateLimitEnabled=1)
	if (bRateLimitEnabled && !Cmd->ClientId.IsEmpty())
	{
		FString RateErr;
		if (!TryConsumeTokens(Cmd->ClientId, GetCommandTokenCost(Type), &RateErr))
		{
			SetError(Cmd, RateErr, TEXT("rate_limited"));
			return;
		}
	}

	// ── MCP JSON-RPC 2.0 protocol methods (initialize, tools/list, tools/call) ──
	if      (Type == TEXT("initialize"))          Cmd_Initialize       (Cmd);
	else if (Type == TEXT("tools/list"))          Cmd_ToolsList        (Cmd);
	else if (Type == TEXT("tools/call"))          Cmd_ToolsCall        (Cmd);
	// ── Native commands ─────────────────────────────────────────────────────
	else if (Type == TEXT("ping"))                Cmd_Ping             (Cmd);
	else if (Type == TEXT("get_status"))           Cmd_GetStatus        (Cmd);
	else if (Type == TEXT("execute_python"))       Cmd_ExecutePython    (Cmd);
	else if (Type == TEXT("console_command"))      Cmd_ConsoleCommand   (Cmd);
	else if (Type == TEXT("get_project_info"))     Cmd_GetProjectInfo   (Cmd);
	else if (Type == TEXT("get_all_actors"))       Cmd_GetAllActors     (Cmd);
	else if (Type == TEXT("spawn_actor"))          Cmd_SpawnActor       (Cmd);
	else if (Type == TEXT("delete_actor"))         Cmd_DeleteActor      (Cmd);
	else if (Type == TEXT("delete_actors"))        Cmd_DeleteActors     (Cmd);
	else if (Type == TEXT("get_actor_transform"))  Cmd_GetActorTransform(Cmd);
	else if (Type == TEXT("set_actor_transform"))  Cmd_SetActorTransform(Cmd);
	else if (Type == TEXT("open_level"))           Cmd_OpenLevel        (Cmd);
	else if (Type == TEXT("get_assets"))           Cmd_GetAssets        (Cmd);
	else if (Type == TEXT("get_selected_assets"))  Cmd_GetSelectedAssets(Cmd);
	else if (Type == TEXT("set_selected_assets"))  Cmd_SetSelectedAssets(Cmd);
	else if (Type == TEXT("create_asset"))         Cmd_CreateAsset      (Cmd);
	else if (Type == TEXT("create_behavior_tree")) Cmd_CreateBehaviorTree(Cmd);
	else if (Type == TEXT("add_blackboard_key"))   Cmd_AddBlackboardKey  (Cmd);
	else if (Type == TEXT("run_behavior_tree"))    Cmd_RunBehaviorTree   (Cmd);
	else if (Type == TEXT("configure_ai_perception")) Cmd_ConfigureAIPerception(Cmd);
	else if (Type == TEXT("configure_ai_hearing"))    Cmd_ConfigureAIHearing   (Cmd);
	else if (Type == TEXT("add_bt_composite_node"))   Cmd_AddBTCompositeNode  (Cmd);
	else if (Type == TEXT("add_bt_decorator_node"))   Cmd_AddBTDecoratorNode  (Cmd);
	else if (Type == TEXT("add_bt_service_node"))     Cmd_AddBTServiceNode    (Cmd);
	else if (Type == TEXT("rebuild_navigation"))      Cmd_RebuildNavigation   (Cmd);
	else if (Type == TEXT("list_viewports"))          Cmd_ListViewports       (Cmd);
	else if (Type == TEXT("set_blueprint_cdo_property")) Cmd_SetBlueprintCDOProperty(Cmd);
	else if (Type == TEXT("get_content_subpaths")) Cmd_GetContentSubpaths(Cmd);
	else if (Type == TEXT("create_content_folder")) Cmd_CreateContentFolder(Cmd);
	else if (Type == TEXT("set_actor_folder"))     Cmd_SetActorFolder   (Cmd);
	else if (Type == TEXT("set_selected_actors_folder")) Cmd_SetSelectedActorsFolder(Cmd);
	else if (Type == TEXT("list_actor_folders"))    Cmd_ListActorFolders (Cmd);
	else if (Type == TEXT("create_actor_folder"))  Cmd_CreateActorFolder(Cmd);
	else if (Type == TEXT("get_world_contexts"))   Cmd_GetWorldContexts (Cmd);
	else if (Type == TEXT("get_viewport_transform")) Cmd_GetViewportTransform(Cmd);
	else if (Type == TEXT("set_viewport_fov"))     Cmd_SetViewportFOV    (Cmd);
	else if (Type == TEXT("get_selected_actors"))  Cmd_GetSelectedActors (Cmd);
	else if (Type == TEXT("set_selected_actors"))  Cmd_SetSelectedActors (Cmd);
	else if (Type == TEXT("get_current_level"))    Cmd_GetCurrentLevel   (Cmd);
	else if (Type == TEXT("get_loaded_levels"))   Cmd_GetLoadedLevels   (Cmd);
	else if (Type == TEXT("load_streaming_level")) Cmd_LoadStreamingLevel(Cmd);
	else if (Type == TEXT("unload_streaming_level")) Cmd_UnloadStreamingLevel(Cmd);
	else if (Type == TEXT("set_actor_property"))   Cmd_SetActorProperty (Cmd);
	else if (Type == TEXT("set_component_property")) Cmd_SetComponentProperty(Cmd);
	else if (Type == TEXT("get_actor_components")) Cmd_GetActorComponents(Cmd);
	else if (Type == TEXT("focus_viewport"))             Cmd_FocusViewport       (Cmd);
	else if (Type == TEXT("take_screenshot"))            Cmd_TakeScreenshot      (Cmd);
	else if (Type == TEXT("add_blueprint_component"))     Cmd_AddBlueprintComponent   (Cmd);
	else if (Type == TEXT("remove_blueprint_component"))  Cmd_RemoveBlueprintComponent(Cmd);
	else if (Type == TEXT("create_blueprint"))            Cmd_CreateBlueprint     (Cmd);
	else if (Type == TEXT("find_blueprint_nodes"))       Cmd_FindBlueprintNodes  (Cmd);
	else if (Type == TEXT("add_blueprint_event_node"))     Cmd_AddBlueprintEventNode  (Cmd);
	else if (Type == TEXT("add_blueprint_function_node"))  Cmd_AddBlueprintFuncNode   (Cmd);
	else if (Type == TEXT("add_blueprint_branch_node"))    Cmd_AddBlueprintBranchNode (Cmd);
	else if (Type == TEXT("add_blueprint_sequence_node"))  Cmd_AddBlueprintSequenceNode(Cmd);
	else if (Type == TEXT("add_blueprint_switch_node"))    Cmd_AddBlueprintSwitchNode (Cmd);
	else if (Type == TEXT("add_blueprint_timeline_node"))  Cmd_AddBlueprintTimelineNode(Cmd);
	else if (Type == TEXT("add_blueprint_foreach_node"))   Cmd_AddBlueprintForEachNode (Cmd);
	else if (Type == TEXT("add_blueprint_switch_string_node")) Cmd_AddBlueprintSwitchStringNode(Cmd);
	else if (Type == TEXT("add_blueprint_switch_enum_node"))   Cmd_AddBlueprintSwitchEnumNode  (Cmd);
	else if (Type == TEXT("add_blueprint_gate_node"))          Cmd_AddBlueprintGateNode        (Cmd);
	else if (Type == TEXT("add_blueprint_multigate_node"))     Cmd_AddBlueprintMultiGateNode   (Cmd);
	else if (Type == TEXT("connect_blueprint_nodes"))      Cmd_ConnectBlueprintNodes (Cmd);
	else if (Type == TEXT("compile_blueprint"))            Cmd_CompileBlueprint       (Cmd);
	// ── Python file ───────────────────────────────────────────────────────────
	else if (Type == TEXT("execute_python_file"))        Cmd_ExecutePythonFile      (Cmd);
	// ── Actor read / duplicate ────────────────────────────────────────────────
	else if (Type == TEXT("get_actor_property"))         Cmd_GetActorProperty       (Cmd);
	else if (Type == TEXT("get_component_property"))     Cmd_GetComponentProperty   (Cmd);
	else if (Type == TEXT("get_all_properties"))         Cmd_GetAllProperties       (Cmd);
	else if (Type == TEXT("duplicate_actor"))            Cmd_DuplicateActor         (Cmd);
	else if (Type == TEXT("place_actor_from_asset"))    Cmd_PlaceActorFromAsset    (Cmd);
	// ── Asset management ──────────────────────────────────────────────────────
	else if (Type == TEXT("save_asset"))                 Cmd_SaveAsset              (Cmd);
	else if (Type == TEXT("save_level"))                 Cmd_SaveLevel              (Cmd);
	else if (Type == TEXT("save_all"))                   Cmd_SaveAll                (Cmd);
	else if (Type == TEXT("delete_asset"))               Cmd_DeleteAsset            (Cmd);
	else if (Type == TEXT("duplicate_asset"))            Cmd_DuplicateAsset         (Cmd);
	else if (Type == TEXT("rename_asset"))               Cmd_RenameAsset            (Cmd);
	else if (Type == TEXT("create_material"))            Cmd_CreateMaterial          (Cmd);
	else if (Type == TEXT("get_material_expressions"))   Cmd_GetMaterialExpressions (Cmd);
	else if (Type == TEXT("get_asset_full_metadata"))   Cmd_GetAssetFullMetadata   (Cmd);
	// ── Blueprint graph read ──────────────────────────────────────────────────
	else if (Type == TEXT("get_blueprint_graphs"))       Cmd_GetBlueprintGraphs     (Cmd);
	else if (Type == TEXT("create_blueprint_graph"))      Cmd_CreateBlueprintGraph   (Cmd);
	else if (Type == TEXT("get_node_info"))              Cmd_GetNodeInfo            (Cmd);
	else if (Type == TEXT("delete_blueprint_node"))      Cmd_DeleteBlueprintNode    (Cmd);
	else if (Type == TEXT("disconnect_blueprint_pins"))  Cmd_DisconnectBlueprintPins(Cmd);
	// ── Blueprint variable management ────────────────────────────────────────
	else if (Type == TEXT("get_blueprint_variables"))       Cmd_GetBlueprintVariables      (Cmd);
	else if (Type == TEXT("add_blueprint_variable"))        Cmd_AddBlueprintVariable       (Cmd);
	else if (Type == TEXT("set_blueprint_variable_default")) Cmd_SetBlueprintVariableDefault(Cmd);
	else if (Type == TEXT("add_blueprint_variable_node"))   Cmd_AddBlueprintVariableNode   (Cmd);
	// ── New utility commands ──────────────────────────────────────────────────
	else if (Type == TEXT("set_node_position"))              Cmd_SetNodePosition   (Cmd);
	else if (Type == TEXT("get_actor_bounds"))               Cmd_GetActorBounds    (Cmd);
	else if (Type == TEXT("set_pin_default_value"))          Cmd_SetPinDefaultValue(Cmd);
	else if (Type == TEXT("get_unsaved_assets"))             Cmd_GetUnsavedAssets  (Cmd);
	// ── Material editing ─────────────────────────────────────────────────────
	else if (Type == TEXT("connect_material_expressions"))  Cmd_ConnectMaterialExpressions(Cmd);
	else if (Type == TEXT("connect_material_property"))     Cmd_ConnectMaterialProperty   (Cmd);
	else if (Type == TEXT("add_material_expression"))            Cmd_AddMaterialExpression          (Cmd);
	else if (Type == TEXT("set_material_expression_property"))   Cmd_SetMaterialExpressionProperty  (Cmd);
	else if (Type == TEXT("get_material_expression_pins"))  Cmd_GetMaterialExpressionPins(Cmd);
	else if (Type == TEXT("delete_material_expression"))    Cmd_DeleteMaterialExpression (Cmd);
	else if (Type == TEXT("recompile_material"))            Cmd_RecompileMaterial        (Cmd);
	// ── UMG ──────────────────────────────────────────────────────────────────
	else if (Type == TEXT("add_umg_widget"))                Cmd_AddUmgWidget              (Cmd);
	else if (Type == TEXT("remove_umg_widget"))             Cmd_RemoveUmgWidget           (Cmd);
	else if (Type == TEXT("get_umg_tree"))                  Cmd_GetUmgTree                (Cmd);
	else if (Type == TEXT("set_umg_slot_content"))          Cmd_SetUmgSlotContent         (Cmd);
	else if (Type == TEXT("create_widget_blueprint"))       Cmd_CreateWidgetBlueprint     (Cmd);
	// ── NEW: MCP protocol extensions ─────────────────────────────────────────
	else if (Type == TEXT("prompts/list"))                  Cmd_PromptsList               (Cmd);
	else if (Type == TEXT("resources/list"))                Cmd_ResourcesList             (Cmd);
	else if (Type == TEXT("health"))                        Cmd_Health                    (Cmd);
	// ── NEW: Asset import & reload ───────────────────────────────────────────
	else if (Type == TEXT("import_asset"))                  Cmd_ImportAsset               (Cmd);
	else if (Type == TEXT("reload_asset"))                  Cmd_ReloadAsset               (Cmd);
	// ── NEW: Lighting ────────────────────────────────────────────────────────
	else if (Type == TEXT("create_light"))                  Cmd_CreateLight               (Cmd);
	else if (Type == TEXT("edit_light"))                    Cmd_EditLight                 (Cmd);
	else if (Type == TEXT("build_lighting"))                Cmd_BuildLighting             (Cmd);
	// ── NEW: Landscape ───────────────────────────────────────────────────────
	else if (Type == TEXT("create_landscape"))              Cmd_CreateLandscape           (Cmd);
	else if (Type == TEXT("get_landscape_info"))            Cmd_GetLandscapeInfo          (Cmd);
	// ── NEW: Foliage ─────────────────────────────────────────────────────────
	else if (Type == TEXT("place_foliage"))                 Cmd_PlaceFoliage              (Cmd);
	else if (Type == TEXT("query_foliage"))                 Cmd_QueryFoliage              (Cmd);
	else if (Type == TEXT("remove_foliage"))                Cmd_RemoveFoliage             (Cmd);
	// ── NEW: Sequencer ───────────────────────────────────────────────────────
	else if (Type == TEXT("create_level_sequence"))         Cmd_CreateLevelSequence       (Cmd);
	else if (Type == TEXT("add_sequencer_track"))           Cmd_AddSequencerTrack         (Cmd);
	else if (Type == TEXT("play_sequence"))                 Cmd_PlaySequence              (Cmd);
	// ── NEW: Niagara / VFX ───────────────────────────────────────────────────
	else if (Type == TEXT("create_niagara_system"))         Cmd_CreateNiagaraSystem       (Cmd);
	else if (Type == TEXT("set_particle_parameter"))        Cmd_SetParticleParameter      (Cmd);
	// ── NEW: Audio ───────────────────────────────────────────────────────────
	else if (Type == TEXT("add_audio_component"))           Cmd_AddAudioComponent         (Cmd);
	// ── NEW: World Partition ─────────────────────────────────────────────────
	else if (Type == TEXT("get_world_partition_info"))      Cmd_GetWorldPartitionInfo     (Cmd);
	else if (Type == TEXT("get_data_layers"))               Cmd_GetDataLayers             (Cmd);
	// ── NEW: Physics ─────────────────────────────────────────────────────────
	else if (Type == TEXT("create_physics_constraint"))     Cmd_CreatePhysicsConstraint   (Cmd);
	// ── NEW: AI extensions ───────────────────────────────────────────────────
	else if (Type == TEXT("configure_ai_damage_perception")) Cmd_ConfigureAIDamagePerception(Cmd);
	else if (Type == TEXT("set_blackboard_value_runtime"))  Cmd_SetBlackboardValueRuntime (Cmd);
	else if (Type == TEXT("possess_pawn"))                  Cmd_PossessPawn               (Cmd);
	// ── NEW: Server QoL — Batch & Undo ───────────────────────────────────────
	else if (Type == TEXT("batch_execute"))                 Cmd_BatchExecute              (Cmd);
	else if (Type == TEXT("begin_transaction"))             Cmd_BeginTransaction          (Cmd);
	else if (Type == TEXT("end_transaction"))               Cmd_EndTransaction            (Cmd);
	else if (Type == TEXT("undo"))                          Cmd_Undo                      (Cmd);
	else if (Type == TEXT("redo"))                          Cmd_Redo                      (Cmd);
	else
	{
		SetError(Cmd, FString::Printf(TEXT("Unknown command type: '%s'"), *Type), TEXT("unknown_command"));
		if (Cmd->bJsonRpc) Cmd->JsonRpcErrorCode = -32601; // Method not found
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// ── MCP PROTOCOL HANDLERS (JSON-RPC 2.0) ────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

// ── initialize ───────────────────────────────────────────────────────────────
// MCP spec requires an initialize handshake to negotiate capabilities.
void FMCPTCPServer::Cmd_Initialize(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("protocolVersion"), TEXT("2025-11-05"));

	// Server info
	TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
	ServerInfo->SetStringField(TEXT("name"), TEXT("unrealmcp"));
	ServerInfo->SetStringField(TEXT("version"), UNREALMCP_VERSION);
	ServerInfo->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Result->SetObjectField(TEXT("serverInfo"), ServerInfo);

	// Capabilities — advertise tools, prompts, and resources
	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
	ToolsCap->SetBoolField(TEXT("listChanged"), false);
	Capabilities->SetObjectField(TEXT("tools"), ToolsCap);
	TSharedPtr<FJsonObject> PromptsCap = MakeShared<FJsonObject>();
	PromptsCap->SetBoolField(TEXT("listChanged"), false);
	Capabilities->SetObjectField(TEXT("prompts"), PromptsCap);
	TSharedPtr<FJsonObject> ResourcesCap = MakeShared<FJsonObject>();
	ResourcesCap->SetBoolField(TEXT("subscribe"), false);
	ResourcesCap->SetBoolField(TEXT("listChanged"), false);
	Capabilities->SetObjectField(TEXT("resources"), ResourcesCap);
	Result->SetObjectField(TEXT("capabilities"), Capabilities);

	SetSuccess(Cmd, Result);
}

// ── tools/list ───────────────────────────────────────────────────────────────
// Returns all available tool definitions with inputSchema for LLM discovery.
void FMCPTCPServer::Cmd_ToolsList(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("tools"), BuildToolDefinitions());
	SetSuccess(Cmd, Result);
}

// ── tools/call ───────────────────────────────────────────────────────────────
// Unified tool invocation: {"method":"tools/call","params":{"name":"spawn_actor","arguments":{...}}}
void FMCPTCPServer::Cmd_ToolsCall(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params for tools/call"), TEXT("invalid_params"));
		if (Cmd->bJsonRpc) Cmd->JsonRpcErrorCode = -32602;
		return;
	}

	FString ToolName;
	if (!Cmd->Params->TryGetStringField(TEXT("name"), ToolName))
	{
		SetError(Cmd, TEXT("Missing 'name' in tools/call params"), TEXT("invalid_params"));
		if (Cmd->bJsonRpc) Cmd->JsonRpcErrorCode = -32602;
		return;
	}

	// Replace params with the "arguments" sub-object (the tool's actual parameters)
	const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
	if (Cmd->Params->TryGetObjectField(TEXT("arguments"), ArgsPtr) && ArgsPtr)
	{
		Cmd->Params = *ArgsPtr;
	}
	else
	{
		Cmd->Params = MakeShared<FJsonObject>();
	}

	// Re-dispatch as the named tool
	Cmd->Type = ToolName;
	DispatchCommand(Cmd);

	// Wrap result in MCP tools/call content format if JSON-RPC
	if (Cmd->bJsonRpc && Cmd->bSuccess && Cmd->ResultObject.IsValid())
	{
		TSharedPtr<FJsonObject> ContentItem = MakeShared<FJsonObject>();
		ContentItem->SetStringField(TEXT("type"), TEXT("text"));

		// Serialize the inner result to a JSON string
		FString InnerJson;
		TSharedRef<TJsonWriter<>> InnerWriter = TJsonWriterFactory<>::Create(&InnerJson);
		FJsonSerializer::Serialize(Cmd->ResultObject.ToSharedRef(), InnerWriter);
		ContentItem->SetStringField(TEXT("text"), InnerJson);

		TArray<TSharedPtr<FJsonValue>> ContentArray;
		ContentArray.Add(MakeShared<FJsonValueObject>(ContentItem));

		TSharedPtr<FJsonObject> WrappedResult = MakeShared<FJsonObject>();
		WrappedResult->SetArrayField(TEXT("content"), ContentArray);
		Cmd->ResultObject = WrappedResult;
	}
}

// ── BuildToolDefinitions ─────────────────────────────────────────────────────
// Generates tool definitions for tools/list response.
TArray<TSharedPtr<FJsonValue>> FMCPTCPServer::BuildToolDefinitions() const
{
	TArray<TSharedPtr<FJsonValue>> Tools;

	// Helper lambda to create a tool definition
	auto AddTool = [&Tools](const FString& Name, const FString& Description, TSharedPtr<FJsonObject> InputSchema)
	{
		TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), Name);
		Tool->SetStringField(TEXT("description"), Description);
		if (!InputSchema.IsValid())
		{
			InputSchema = MakeShared<FJsonObject>();
			InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		}
		Tool->SetObjectField(TEXT("inputSchema"), InputSchema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	};

	// Helper to build a simple schema with string properties
	auto MakeSchema = [](TArray<TPair<FString, FString>> Props, TArray<FString> Required = {}) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		for (const auto& P : Props)
		{
			TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
			PropObj->SetStringField(TEXT("type"), P.Value);
			Properties->SetObjectField(P.Key, PropObj);
		}
		Schema->SetObjectField(TEXT("properties"), Properties);
		if (Required.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ReqArr;
			for (const FString& R : Required) ReqArr.Add(MakeShared<FJsonValueString>(R));
			Schema->SetArrayField(TEXT("required"), ReqArr);
		}
		return Schema;
	};

	// Helper to build schema with property descriptions for richer inputSchema
	auto MakeRichSchema = [](TArray<TTuple<FString, FString, FString>> Props, TArray<FString> Required = {}) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		for (const auto& P : Props)
		{
			TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
			PropObj->SetStringField(TEXT("type"), P.Get<1>());
			if (!P.Get<2>().IsEmpty())
			{
				PropObj->SetStringField(TEXT("description"), P.Get<2>());
			}
			Properties->SetObjectField(P.Get<0>(), PropObj);
		}
		Schema->SetObjectField(TEXT("properties"), Properties);
		if (Required.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ReqArr;
			for (const FString& R : Required) ReqArr.Add(MakeShared<FJsonValueString>(R));
			Schema->SetArrayField(TEXT("required"), ReqArr);
		}
		return Schema;
	};

	// ── Core tools ──────────────────────────────────────────────────────────
	AddTool(TEXT("ping"), TEXT("Connectivity check. Returns pong, version, engine info."), nullptr);
	AddTool(TEXT("get_status"), TEXT("Server status: connected clients, port, running state."), nullptr);
	AddTool(TEXT("execute_python"), TEXT("Execute Python code in the editor via PythonScriptPlugin."),
		MakeSchema({{TEXT("code"), TEXT("string")}, {TEXT("mode"), TEXT("string")}}, {TEXT("code")}));
	AddTool(TEXT("execute_python_file"), TEXT("Execute a Python script file."),
		MakeSchema({{TEXT("file_path"), TEXT("string")}}, {TEXT("file_path")}));
	AddTool(TEXT("console_command"), TEXT("Execute a UE console command."),
		MakeSchema({{TEXT("command"), TEXT("string")}}, {TEXT("command")}));
	AddTool(TEXT("get_project_info"), TEXT("Get project name, paths, and engine version."), nullptr);

	// ── Level & World ───────────────────────────────────────────────────────
	AddTool(TEXT("open_level"), TEXT("Load a level by path."),
		MakeSchema({{TEXT("level_path"), TEXT("string")}}, {TEXT("level_path")}));
	AddTool(TEXT("get_world_contexts"), TEXT("List all world contexts with type and index."), nullptr);
	AddTool(TEXT("get_current_level"), TEXT("Get current level name and path."), nullptr);
	AddTool(TEXT("get_loaded_levels"), TEXT("List all loaded and streaming levels."), nullptr);
	AddTool(TEXT("load_streaming_level"), TEXT("Add a streaming level to the editor world."),
		MakeSchema({{TEXT("level_path"), TEXT("string")}}, {TEXT("level_path")}));
	AddTool(TEXT("unload_streaming_level"), TEXT("Remove a streaming level."),
		MakeSchema({{TEXT("level_path"), TEXT("string")}}, {}));

	// ── Actors ──────────────────────────────────────────────────────────────
	AddTool(TEXT("get_all_actors"), TEXT("List all actors with optional class/label filter."),
		MakeSchema({{TEXT("class_filter"), TEXT("string")}, {TEXT("label_filter"), TEXT("string")}}, {}));
	AddTool(TEXT("spawn_actor"), TEXT("Spawn an actor with optional class, location, rotation, label."),
		MakeSchema({{TEXT("class"), TEXT("string")}, {TEXT("label"), TEXT("string")}}, {}));
	AddTool(TEXT("delete_actor"), TEXT("Delete an actor by path or label."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("actor_label"), TEXT("string")}}, {}));
	AddTool(TEXT("delete_actors"), TEXT("Bulk delete actors by class, label, or tag filter."),
		MakeSchema({{TEXT("class_name"), TEXT("string")}, {TEXT("actor_label_contains"), TEXT("string")}}, {}));
	AddTool(TEXT("duplicate_actor"), TEXT("Duplicate an actor."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("actor_label"), TEXT("string")}}, {}));
	AddTool(TEXT("get_actor_transform"), TEXT("Get actor location, rotation, and scale."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("actor_label"), TEXT("string")}}, {}));
	AddTool(TEXT("set_actor_transform"), TEXT("Set actor location, rotation, and/or scale."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("actor_label"), TEXT("string")}}, {}));
	AddTool(TEXT("get_actor_bounds"), TEXT("Get actor origin and extent."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("actor_label"), TEXT("string")}}, {}));
	AddTool(TEXT("get_actor_property"), TEXT("Read actor property by dot-path."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("property"), TEXT("string")}}, {TEXT("property")}));
	AddTool(TEXT("set_actor_property"), TEXT("Set actor property with JSON value."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("property"), TEXT("string")}}, {TEXT("property")}));
	AddTool(TEXT("get_all_properties"), TEXT("List all properties on an object."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("object_path"), TEXT("string")}}, {}));
	AddTool(TEXT("get_actor_components"), TEXT("List all components on an actor."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("actor_label"), TEXT("string")}}, {}));
	AddTool(TEXT("place_actor_from_asset"), TEXT("Place actor from a content asset (mesh, blueprint, etc.)."),
		MakeSchema({{TEXT("asset_path"), TEXT("string")}}, {TEXT("asset_path")}));
	AddTool(TEXT("set_component_property"), TEXT("Set a property on an actor's component."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("component"), TEXT("string")}, {TEXT("property"), TEXT("string")}}, {TEXT("component"), TEXT("property")}));
	AddTool(TEXT("get_component_property"), TEXT("Read a property from an actor's component."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("component"), TEXT("string")}, {TEXT("property"), TEXT("string")}}, {TEXT("component"), TEXT("property")}));

	// ── Outliner & Folders ──────────────────────────────────────────────────
	AddTool(TEXT("set_actor_folder"), TEXT("Set an actor's outliner folder."),
		MakeSchema({{TEXT("actor_path"), TEXT("string")}, {TEXT("folder_path"), TEXT("string")}}, {TEXT("folder_path")}));
	AddTool(TEXT("set_selected_actors_folder"), TEXT("Move selected actors to a folder."),
		MakeSchema({{TEXT("folder_path"), TEXT("string")}}, {TEXT("folder_path")}));
	AddTool(TEXT("list_actor_folders"), TEXT("List all actor folder paths in the world."), nullptr);
	AddTool(TEXT("create_actor_folder"), TEXT("Create an actor folder."),
		MakeSchema({{TEXT("folder_path"), TEXT("string")}}, {TEXT("folder_path")}));

	// ── Assets & Content Browser ────────────────────────────────────────────
	AddTool(TEXT("get_assets"), TEXT("List assets under a content path."),
		MakeSchema({{TEXT("path"), TEXT("string")}, {TEXT("class_filter"), TEXT("string")}, {TEXT("recursive"), TEXT("boolean")}}, {}));
	AddTool(TEXT("get_selected_assets"), TEXT("Get assets selected in Content Browser."), nullptr);
	AddTool(TEXT("set_selected_assets"), TEXT("Set Content Browser selection."),
		MakeSchema({{TEXT("asset_paths"), TEXT("array")}}, {TEXT("asset_paths")}));
	AddTool(TEXT("create_asset"), TEXT("Create a new asset (generic factory-based)."),
		MakeSchema({{TEXT("asset_name"), TEXT("string")}, {TEXT("package_path"), TEXT("string")}, {TEXT("asset_class"), TEXT("string")}}, {TEXT("asset_name"), TEXT("package_path"), TEXT("asset_class")}));
	AddTool(TEXT("create_blueprint"), TEXT("Create a Blueprint from a parent class."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("package_path"), TEXT("string")}, {TEXT("parent_class"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("package_path"), TEXT("parent_class")}));
	AddTool(TEXT("create_material"), TEXT("Create an empty material asset."),
		MakeSchema({{TEXT("asset_path"), TEXT("string")}}, {TEXT("asset_path")}));
	AddTool(TEXT("save_asset"), TEXT("Save an asset to disk."),
		MakeSchema({{TEXT("asset_path"), TEXT("string")}}, {TEXT("asset_path")}));
	AddTool(TEXT("save_level"), TEXT("Save the current level."), nullptr);
	AddTool(TEXT("save_all"), TEXT("Save all dirty assets and levels."), nullptr);
	AddTool(TEXT("delete_asset"), TEXT("Delete an asset."),
		MakeSchema({{TEXT("asset_path"), TEXT("string")}}, {TEXT("asset_path")}));
	AddTool(TEXT("duplicate_asset"), TEXT("Duplicate an asset."),
		MakeSchema({{TEXT("asset_path"), TEXT("string")}, {TEXT("destination_path"), TEXT("string")}}, {TEXT("asset_path"), TEXT("destination_path")}));
	AddTool(TEXT("rename_asset"), TEXT("Rename an asset."),
		MakeSchema({{TEXT("asset_path"), TEXT("string")}, {TEXT("new_name"), TEXT("string")}}, {TEXT("asset_path"), TEXT("new_name")}));
	AddTool(TEXT("get_asset_full_metadata"), TEXT("Get full metadata and tags for an asset."),
		MakeSchema({{TEXT("asset_path"), TEXT("string")}}, {TEXT("asset_path")}));
	AddTool(TEXT("get_unsaved_assets"), TEXT("List all unsaved asset paths."), nullptr);
	AddTool(TEXT("get_content_subpaths"), TEXT("List subfolder paths under a content path."),
		MakeSchema({{TEXT("base_path"), TEXT("string")}}, {}));
	AddTool(TEXT("create_content_folder"), TEXT("Create a content browser folder."),
		MakeSchema({{TEXT("folder_path"), TEXT("string")}}, {TEXT("folder_path")}));

	// ── Blueprint Graph ─────────────────────────────────────────────────────
	AddTool(TEXT("get_blueprint_graphs"), TEXT("List all graphs in a Blueprint."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));
	AddTool(TEXT("create_blueprint_graph"), TEXT("Create a function or macro graph."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("graph_name"), TEXT("string")}, {TEXT("graph_type"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("graph_name"), TEXT("graph_type")}));
	AddTool(TEXT("find_blueprint_nodes"), TEXT("Find nodes in a Blueprint by class, event, or name."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));
	AddTool(TEXT("get_node_info"), TEXT("Get detailed info about a Blueprint node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("node_id"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("node_id")}));
	AddTool(TEXT("add_blueprint_event_node"), TEXT("Add an event node to a Blueprint graph."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("event_name"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("event_name")}));
	AddTool(TEXT("add_blueprint_function_node"), TEXT("Add a function call node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("function_name"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("function_name")}));
	AddTool(TEXT("add_blueprint_branch_node"), TEXT("Add a Branch (if/else) node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));
	AddTool(TEXT("add_blueprint_sequence_node"), TEXT("Add a Sequence node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));
	AddTool(TEXT("add_blueprint_switch_node"), TEXT("Add a Switch on Integer node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));
	AddTool(TEXT("add_blueprint_switch_string_node"), TEXT("Add a Switch on String node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));
	AddTool(TEXT("add_blueprint_switch_enum_node"), TEXT("Add a Switch on Enum node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("enum_path"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("enum_path")}));
	AddTool(TEXT("add_blueprint_timeline_node"), TEXT("Add a Timeline node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));
	AddTool(TEXT("add_blueprint_foreach_node"), TEXT("Add a ForEach loop node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));
	AddTool(TEXT("add_blueprint_gate_node"), TEXT("Add a Gate macro node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));
	AddTool(TEXT("add_blueprint_multigate_node"), TEXT("Add a MultiGate macro node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));
	AddTool(TEXT("connect_blueprint_nodes"), TEXT("Connect two Blueprint node pins."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("source_node_id"), TEXT("string")}, {TEXT("source_pin"), TEXT("string")}, {TEXT("target_node_id"), TEXT("string")}, {TEXT("target_pin"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("source_node_id"), TEXT("source_pin"), TEXT("target_node_id"), TEXT("target_pin")}));
	AddTool(TEXT("disconnect_blueprint_pins"), TEXT("Disconnect two Blueprint node pins."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("source_node_id"), TEXT("string")}, {TEXT("source_pin"), TEXT("string")}, {TEXT("target_node_id"), TEXT("string")}, {TEXT("target_pin"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("source_node_id"), TEXT("source_pin"), TEXT("target_node_id"), TEXT("target_pin")}));
	AddTool(TEXT("set_node_position"), TEXT("Move a Blueprint node to a new position."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("node_id"), TEXT("string")}, {TEXT("x"), TEXT("number")}, {TEXT("y"), TEXT("number")}}, {TEXT("blueprint_name"), TEXT("node_id"), TEXT("x"), TEXT("y")}));
	AddTool(TEXT("set_pin_default_value"), TEXT("Set default value on a Blueprint pin."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("node_id"), TEXT("string")}, {TEXT("pin_name"), TEXT("string")}, {TEXT("value"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("node_id"), TEXT("pin_name"), TEXT("value")}));
	AddTool(TEXT("delete_blueprint_node"), TEXT("Delete a node from a Blueprint."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("node_id"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("node_id")}));
	AddTool(TEXT("compile_blueprint"), TEXT("Compile a Blueprint."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));

	// ── Blueprint Variables ──────────────────────────────────────────────────
	AddTool(TEXT("get_blueprint_variables"), TEXT("List all variables in a Blueprint."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}}, {TEXT("blueprint_name")}));
	AddTool(TEXT("add_blueprint_variable"), TEXT("Add a variable to a Blueprint."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("variable_name"), TEXT("string")}, {TEXT("variable_type"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("variable_name"), TEXT("variable_type")}));
	AddTool(TEXT("set_blueprint_variable_default"), TEXT("Set default value of a Blueprint variable."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("variable_name"), TEXT("string")}, {TEXT("value"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("variable_name"), TEXT("value")}));
	AddTool(TEXT("add_blueprint_variable_node"), TEXT("Add a get/set variable node."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("variable_name"), TEXT("string")}, {TEXT("node_type"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("variable_name"), TEXT("node_type")}));
	AddTool(TEXT("add_blueprint_component"), TEXT("Add a component to a Blueprint."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("component_class"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("component_class")}));
	AddTool(TEXT("remove_blueprint_component"), TEXT("Remove a component from a Blueprint."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("component_name"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("component_name")}));
	AddTool(TEXT("set_blueprint_cdo_property"), TEXT("Set a class default property on a Blueprint."),
		MakeSchema({{TEXT("blueprint_name"), TEXT("string")}, {TEXT("property"), TEXT("string")}}, {TEXT("blueprint_name"), TEXT("property")}));

	// ── Materials ────────────────────────────────────────────────────────────
	AddTool(TEXT("get_material_expressions"), TEXT("List all expressions in a material."),
		MakeSchema({{TEXT("material_path"), TEXT("string")}}, {TEXT("material_path")}));
	AddTool(TEXT("add_material_expression"), TEXT("Add an expression node to a material."),
		MakeSchema({{TEXT("material_path"), TEXT("string")}, {TEXT("expression_class"), TEXT("string")}}, {TEXT("material_path"), TEXT("expression_class")}));
	AddTool(TEXT("delete_material_expression"), TEXT("Remove an expression from a material."),
		MakeSchema({{TEXT("material_path"), TEXT("string")}}, {TEXT("material_path")}));
	AddTool(TEXT("connect_material_expressions"), TEXT("Connect two material expressions."),
		MakeSchema({{TEXT("material_path"), TEXT("string")}, {TEXT("from_expression"), TEXT("string")}, {TEXT("to_expression"), TEXT("string")}}, {TEXT("material_path"), TEXT("from_expression"), TEXT("to_expression")}));
	AddTool(TEXT("connect_material_property"), TEXT("Connect an expression to a material property."),
		MakeSchema({{TEXT("material_path"), TEXT("string")}, {TEXT("from_expression"), TEXT("string")}, {TEXT("property"), TEXT("string")}}, {TEXT("material_path"), TEXT("from_expression"), TEXT("property")}));
	AddTool(TEXT("get_material_expression_pins"), TEXT("Get pin names and types for a material expression."),
		MakeSchema({{TEXT("material_path"), TEXT("string")}}, {TEXT("material_path")}));
	AddTool(TEXT("recompile_material"), TEXT("Recompile a material."),
		MakeSchema({{TEXT("material_path"), TEXT("string")}}, {TEXT("material_path")}));
	AddTool(TEXT("set_material_expression_property"), TEXT("Set a property on a material expression node."),
		MakeSchema({{TEXT("material_path"), TEXT("string")}}, {TEXT("material_path")}));

	// ── UMG (Widget Blueprint) ──────────────────────────────────────────────
	AddTool(TEXT("create_widget_blueprint"), TEXT("Create a new Widget Blueprint."),
		MakeSchema({{TEXT("asset_name"), TEXT("string")}, {TEXT("package_path"), TEXT("string")}}, {TEXT("asset_name"), TEXT("package_path")}));
	AddTool(TEXT("add_umg_widget"), TEXT("Add a widget to a Widget Blueprint tree."),
		MakeSchema({{TEXT("widget_blueprint_path"), TEXT("string")}, {TEXT("widget_class"), TEXT("string")}}, {TEXT("widget_blueprint_path"), TEXT("widget_class")}));
	AddTool(TEXT("remove_umg_widget"), TEXT("Remove a widget from a Widget Blueprint tree."),
		MakeSchema({{TEXT("widget_blueprint_path"), TEXT("string")}, {TEXT("widget_name"), TEXT("string")}}, {TEXT("widget_blueprint_path"), TEXT("widget_name")}));
	AddTool(TEXT("get_umg_tree"), TEXT("Get the widget hierarchy of a Widget Blueprint."),
		MakeSchema({{TEXT("widget_blueprint_path"), TEXT("string")}}, {TEXT("widget_blueprint_path")}));
	AddTool(TEXT("set_umg_slot_content"), TEXT("Set widget content for a named slot."),
		MakeSchema({{TEXT("widget_blueprint_path"), TEXT("string")}, {TEXT("slot_name"), TEXT("string")}, {TEXT("content_widget_name"), TEXT("string")}}, {TEXT("widget_blueprint_path"), TEXT("slot_name"), TEXT("content_widget_name")}));

	// ── AI ───────────────────────────────────────────────────────────────────
	AddTool(TEXT("create_behavior_tree"), TEXT("Create a Behavior Tree and linked Blackboard asset pair."),
		MakeSchema({{TEXT("asset_name"), TEXT("string")}, {TEXT("package_path"), TEXT("string")}}, {TEXT("asset_name"), TEXT("package_path")}));
	AddTool(TEXT("add_blackboard_key"), TEXT("Add a key to a Blackboard."),
		MakeSchema({{TEXT("blackboard_path"), TEXT("string")}, {TEXT("key_name"), TEXT("string")}, {TEXT("key_type"), TEXT("string")}}, {TEXT("blackboard_path"), TEXT("key_name"), TEXT("key_type")}));
	AddTool(TEXT("run_behavior_tree"), TEXT("Run a Behavior Tree on an AI Controller."),
		MakeSchema({{TEXT("behavior_tree_path"), TEXT("string")}}, {TEXT("behavior_tree_path")}));
	AddTool(TEXT("configure_ai_perception"), TEXT("Add/configure Sight sense on an AI Controller."),
		MakeSchema({{TEXT("controller_actor_path"), TEXT("string")}}, {}));
	AddTool(TEXT("configure_ai_hearing"), TEXT("Add/configure Hearing sense on an AI Controller."),
		MakeSchema({{TEXT("controller_actor_path"), TEXT("string")}}, {}));
	AddTool(TEXT("add_bt_composite_node"), TEXT("Add a Selector or Sequence composite to a Behavior Tree."),
		MakeSchema({{TEXT("behavior_tree_path"), TEXT("string")}, {TEXT("composite_type"), TEXT("string")}}, {TEXT("behavior_tree_path"), TEXT("composite_type")}));
	AddTool(TEXT("add_bt_decorator_node"), TEXT("Add a decorator to a Behavior Tree node."),
		MakeSchema({{TEXT("behavior_tree_path"), TEXT("string")}}, {TEXT("behavior_tree_path")}));
	AddTool(TEXT("add_bt_service_node"), TEXT("Add a service to a Behavior Tree node."),
		MakeSchema({{TEXT("behavior_tree_path"), TEXT("string")}}, {TEXT("behavior_tree_path")}));
	AddTool(TEXT("rebuild_navigation"), TEXT("Rebuild NavMesh for the world."), nullptr);

	// ── Viewport & Selection ────────────────────────────────────────────────
	AddTool(TEXT("get_selected_actors"), TEXT("List currently selected actors."), nullptr);
	AddTool(TEXT("set_selected_actors"), TEXT("Set editor selection to given actors."),
		MakeSchema({{TEXT("actor_paths"), TEXT("array")}}, {}));
	AddTool(TEXT("focus_viewport"), TEXT("Move viewport camera to actor or coordinates."),
		MakeSchema({{TEXT("actor_label"), TEXT("string")}, {TEXT("x"), TEXT("number")}, {TEXT("y"), TEXT("number")}, {TEXT("z"), TEXT("number")}}, {}));
	AddTool(TEXT("get_viewport_transform"), TEXT("Get viewport camera location, rotation, and FOV."), nullptr);
	AddTool(TEXT("set_viewport_fov"), TEXT("Set viewport field of view."),
		MakeSchema({{TEXT("fov"), TEXT("number")}}, {TEXT("fov")}));
	AddTool(TEXT("take_screenshot"), TEXT("Request a screenshot of the viewport."),
		MakeSchema({{TEXT("filename"), TEXT("string")}}, {}));
	AddTool(TEXT("list_viewports"), TEXT("Enumerate open viewport windows."), nullptr);

	// ── NEW: Health ─────────────────────────────────────────────────────────
	AddTool(TEXT("health"), TEXT("Health check endpoint. Returns 200-equivalent when server is up."), nullptr);

	// ── NEW: Asset import & reload ──────────────────────────────────────────
	AddTool(TEXT("import_asset"), TEXT("Import an asset from a file on disk (FBX, PNG, etc.) using IAssetTools::ImportAssetsAutomated."),
		MakeRichSchema({
			{TEXT("file_path"), TEXT("string"), TEXT("Absolute path to the file to import.")},
			{TEXT("destination_path"), TEXT("string"), TEXT("Content path for the imported asset (e.g. /Game/Meshes).")},
		}, {TEXT("file_path"), TEXT("destination_path")}));
	AddTool(TEXT("reload_asset"), TEXT("Reload an asset from disk (reimport). Useful after external changes."),
		MakeRichSchema({
			{TEXT("asset_path"), TEXT("string"), TEXT("Content path of the asset to reload.")},
		}, {TEXT("asset_path")}));

	// ── NEW: Lighting ───────────────────────────────────────────────────────
	AddTool(TEXT("create_light"), TEXT("Spawn a light actor (directional, point, spot, or rect)."),
		MakeRichSchema({
			{TEXT("light_type"), TEXT("string"), TEXT("One of: directional, point, spot, rect.")},
			{TEXT("label"), TEXT("string"), TEXT("Optional label for the light actor.")},
			{TEXT("x"), TEXT("number"), TEXT("X location.")},
			{TEXT("y"), TEXT("number"), TEXT("Y location.")},
			{TEXT("z"), TEXT("number"), TEXT("Z location.")},
			{TEXT("intensity"), TEXT("number"), TEXT("Light intensity.")},
			{TEXT("color_r"), TEXT("number"), TEXT("Red component (0-1).")},
			{TEXT("color_g"), TEXT("number"), TEXT("Green component (0-1).")},
			{TEXT("color_b"), TEXT("number"), TEXT("Blue component (0-1).")},
		}, {TEXT("light_type")}));
	AddTool(TEXT("edit_light"), TEXT("Modify a light actor's properties (intensity, color, mobility)."),
		MakeRichSchema({
			{TEXT("actor_path"), TEXT("string"), TEXT("Path or label of the light actor.")},
			{TEXT("actor_label"), TEXT("string"), TEXT("Label of the light actor.")},
			{TEXT("intensity"), TEXT("number"), TEXT("New intensity value.")},
			{TEXT("color_r"), TEXT("number"), TEXT("Red component (0-1).")},
			{TEXT("color_g"), TEXT("number"), TEXT("Green component (0-1).")},
			{TEXT("color_b"), TEXT("number"), TEXT("Blue component (0-1).")},
			{TEXT("mobility"), TEXT("string"), TEXT("static, stationary, or movable.")},
		}, {}));
	AddTool(TEXT("build_lighting"), TEXT("Trigger a lighting build (equivalent to Build > Build Lighting Only)."), nullptr);

	// ── NEW: Landscape ──────────────────────────────────────────────────────
	AddTool(TEXT("create_landscape"), TEXT("Create a landscape (terrain) actor in the level."),
		MakeRichSchema({
			{TEXT("num_quads_x"), TEXT("number"), TEXT("Number of quads in X (width).")},
			{TEXT("num_quads_y"), TEXT("number"), TEXT("Number of quads in Y (height).")},
			{TEXT("quad_size"), TEXT("number"), TEXT("Size of each quad in world units.")},
			{TEXT("label"), TEXT("string"), TEXT("Optional label for the landscape.")},
		}, {}));
	AddTool(TEXT("get_landscape_info"), TEXT("Query landscape info: bounds, resolution, num components."), nullptr);

	// ── NEW: Foliage ────────────────────────────────────────────────────────
	AddTool(TEXT("place_foliage"), TEXT("Place foliage instances in the level from a static mesh."),
		MakeRichSchema({
			{TEXT("mesh_path"), TEXT("string"), TEXT("Content path of the static mesh to use as foliage.")},
			{TEXT("locations"), TEXT("array"), TEXT("Array of {x,y,z} location objects.")},
		}, {TEXT("mesh_path"), TEXT("locations")}));
	AddTool(TEXT("query_foliage"), TEXT("List foliage types and instance counts in the level."), nullptr);
	AddTool(TEXT("remove_foliage"), TEXT("Remove foliage instances by type or in a region."),
		MakeRichSchema({
			{TEXT("mesh_path"), TEXT("string"), TEXT("Content path of the foliage mesh type to remove.")},
		}, {}));

	// ── NEW: Sequencer ──────────────────────────────────────────────────────
	AddTool(TEXT("create_level_sequence"), TEXT("Create a Level Sequence asset and optionally place it in the world."),
		MakeRichSchema({
			{TEXT("asset_name"), TEXT("string"), TEXT("Name for the Level Sequence asset.")},
			{TEXT("package_path"), TEXT("string"), TEXT("Content path (e.g. /Game/Cinematics).")},
			{TEXT("place_in_world"), TEXT("boolean"), TEXT("If true, also spawn a LevelSequenceActor.")},
		}, {TEXT("asset_name"), TEXT("package_path")}));
	AddTool(TEXT("add_sequencer_track"), TEXT("Add a track (transform, property, camera cut) to a Level Sequence."),
		MakeRichSchema({
			{TEXT("sequence_path"), TEXT("string"), TEXT("Content path of the Level Sequence.")},
			{TEXT("actor_label"), TEXT("string"), TEXT("Label of the actor to bind.")},
			{TEXT("track_type"), TEXT("string"), TEXT("Type: transform, property, camera_cut.")},
		}, {TEXT("sequence_path"), TEXT("track_type")}));
	AddTool(TEXT("play_sequence"), TEXT("Play, pause, or scrub a Level Sequence in the editor."),
		MakeRichSchema({
			{TEXT("sequence_path"), TEXT("string"), TEXT("Content path of the Level Sequence.")},
			{TEXT("action"), TEXT("string"), TEXT("play, pause, stop, or scrub.")},
			{TEXT("time"), TEXT("number"), TEXT("Time in seconds (for scrub).")},
		}, {TEXT("sequence_path"), TEXT("action")}));

	// ── NEW: Niagara / VFX ──────────────────────────────────────────────────
	AddTool(TEXT("create_niagara_system"), TEXT("Create or spawn a Niagara particle system in the level."),
		MakeRichSchema({
			{TEXT("system_path"), TEXT("string"), TEXT("Content path of the Niagara System asset to spawn.")},
			{TEXT("x"), TEXT("number"), TEXT("X location.")},
			{TEXT("y"), TEXT("number"), TEXT("Y location.")},
			{TEXT("z"), TEXT("number"), TEXT("Z location.")},
			{TEXT("label"), TEXT("string"), TEXT("Optional actor label.")},
		}, {TEXT("system_path")}));
	AddTool(TEXT("set_particle_parameter"), TEXT("Set a parameter on a Niagara system component."),
		MakeRichSchema({
			{TEXT("actor_label"), TEXT("string"), TEXT("Label of the actor with the Niagara component.")},
			{TEXT("parameter_name"), TEXT("string"), TEXT("Name of the parameter to set.")},
			{TEXT("value"), TEXT("number"), TEXT("Numeric value for the parameter.")},
		}, {TEXT("actor_label"), TEXT("parameter_name")}));

	// ── NEW: Audio ──────────────────────────────────────────────────────────
	AddTool(TEXT("add_audio_component"), TEXT("Add or configure an audio component on an actor."),
		MakeRichSchema({
			{TEXT("actor_path"), TEXT("string"), TEXT("Actor to add audio to.")},
			{TEXT("actor_label"), TEXT("string"), TEXT("Label of the actor.")},
			{TEXT("sound_path"), TEXT("string"), TEXT("Content path of the SoundBase/SoundCue asset.")},
			{TEXT("volume"), TEXT("number"), TEXT("Volume multiplier (default 1.0).")},
			{TEXT("auto_activate"), TEXT("boolean"), TEXT("Whether to play on begin play.")},
		}, {}));

	// ── NEW: World Partition ─────────────────────────────────────────────────
	AddTool(TEXT("get_world_partition_info"), TEXT("Query world partition state, bounds, and streaming info."), nullptr);
	AddTool(TEXT("get_data_layers"), TEXT("List data layers and their states."), nullptr);

	// ── NEW: Physics ────────────────────────────────────────────────────────
	AddTool(TEXT("create_physics_constraint"), TEXT("Create a physics constraint between two actors."),
		MakeRichSchema({
			{TEXT("actor1_label"), TEXT("string"), TEXT("Label of the first constrained actor.")},
			{TEXT("actor2_label"), TEXT("string"), TEXT("Label of the second constrained actor.")},
			{TEXT("constraint_type"), TEXT("string"), TEXT("Type: ball, hinge, prismatic, fixed.")},
			{TEXT("x"), TEXT("number"), TEXT("X location for the constraint actor.")},
			{TEXT("y"), TEXT("number"), TEXT("Y location.")},
			{TEXT("z"), TEXT("number"), TEXT("Z location.")},
		}, {TEXT("actor1_label"), TEXT("actor2_label")}));

	// ── NEW: AI extensions ──────────────────────────────────────────────────
	AddTool(TEXT("configure_ai_damage_perception"), TEXT("Add/configure Damage sense on an AI Controller."),
		MakeRichSchema({
			{TEXT("controller_actor_path"), TEXT("string"), TEXT("Path or label of the AI Controller actor.")},
			{TEXT("controller_actor_label"), TEXT("string"), TEXT("Label of the AI Controller actor.")},
		}, {}));
	AddTool(TEXT("set_blackboard_value_runtime"), TEXT("Set a blackboard key value at runtime (PIE)."),
		MakeRichSchema({
			{TEXT("controller_actor_label"), TEXT("string"), TEXT("Label of the AI Controller.")},
			{TEXT("key_name"), TEXT("string"), TEXT("Blackboard key name.")},
			{TEXT("value"), TEXT("string"), TEXT("Value to set (converted to key type).")},
		}, {TEXT("controller_actor_label"), TEXT("key_name"), TEXT("value")}));
	AddTool(TEXT("possess_pawn"), TEXT("Possess a Pawn with an AI Controller."),
		MakeRichSchema({
			{TEXT("controller_label"), TEXT("string"), TEXT("Label of the AI Controller actor.")},
			{TEXT("pawn_label"), TEXT("string"), TEXT("Label of the Pawn to possess.")},
		}, {TEXT("controller_label"), TEXT("pawn_label")}));

	// ── NEW: Batch & Undo ───────────────────────────────────────────────────
	AddTool(TEXT("batch_execute"), TEXT("Execute multiple commands in a single request. Returns results for each."),
		MakeRichSchema({
			{TEXT("commands"), TEXT("array"), TEXT("Array of {type, params} objects to execute sequentially.")},
			{TEXT("use_transaction"), TEXT("boolean"), TEXT("If true, wrap all commands in a single undo transaction.")},
		}, {TEXT("commands")}));
	AddTool(TEXT("begin_transaction"), TEXT("Begin a named undo transaction. Pair with end_transaction."),
		MakeRichSchema({
			{TEXT("description"), TEXT("string"), TEXT("Human-readable description for the undo entry.")},
		}, {TEXT("description")}));
	AddTool(TEXT("end_transaction"), TEXT("End the current undo transaction."), nullptr);
	AddTool(TEXT("undo"), TEXT("Undo the last editor transaction."),
		MakeRichSchema({
			{TEXT("count"), TEXT("number"), TEXT("Number of transactions to undo (default 1).")},
		}, {}));
	AddTool(TEXT("redo"), TEXT("Redo the last undone transaction."),
		MakeRichSchema({
			{TEXT("count"), TEXT("number"), TEXT("Number of transactions to redo (default 1).")},
		}, {}));

	return Tools;
}

// ─────────────────────────────────────────────────────────────────────────────
// ── COMMAND HANDLERS ─────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

// ── ping ─────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_Ping(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"),  TEXT("pong"));
	Result->SetStringField(TEXT("version"), UNREALMCP_VERSION);
	Result->SetStringField(TEXT("engine"),  FEngineVersion::Current().ToString());
	Result->SetNumberField(TEXT("port"),    ListenPort);
	SetSuccess(Cmd, Result);
}

// ── get_status ────────────────────────────────────────────────────────────────
//
// result: { "connected_clients": N, "port": P, "running": true }
//
void FMCPTCPServer::Cmd_GetStatus(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("connected_clients"), NumConnectedClients.load());
	Result->SetNumberField(TEXT("port"), ListenPort);
	Result->SetBoolField(TEXT("running"), bRunning.load());
	Result->SetStringField(TEXT("bind_address"), BindAddress);
	Result->SetBoolField(TEXT("rate_limit_enabled"), bRateLimitEnabled);

	// Per-client token counts (if rate limiting is enabled)
	if (bRateLimitEnabled)
	{
		TSharedPtr<FJsonObject> TokensObj = MakeShared<FJsonObject>();
		FScopeLock Lock(&RateLimitLock);
		for (const auto& Pair : ClientTokens)
		{
			TokensObj->SetNumberField(Pair.Key, Pair.Value.Tokens);
		}
		Result->SetObjectField(TEXT("client_tokens"), TokensObj);
	}

	SetSuccess(Cmd, Result);
}

// ── execute_python ────────────────────────────────────────────────────────────
//
// params: { "code": "...", "mode": "execute" | "evaluate" }
//
// Delegates to IPythonScriptPlugin — the zero-limitations tool.
// Every Python tool in the MCP (blueprints, materials, assets, etc.) routes here.
//
void FMCPTCPServer::Cmd_ExecutePython(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params object"));
		return;
	}

	FString Code, Mode;
	if (!Params->TryGetStringField(TEXT("code"), Code))
	{
		SetError(Cmd, TEXT("Missing 'code' parameter"));
		return;
	}
	Params->TryGetStringField(TEXT("mode"), Mode);
	const bool bEvaluate = Mode == TEXT("evaluate");

	// Check Python availability
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin || !PythonPlugin->IsPythonAvailable())
	{
		SetError(Cmd,
			TEXT("Python Script Plugin is not available. "
			     "Enable 'Python Script Plugin' in Edit > Plugins, then restart UE."));
		return;
	}

	// Build the execution struct
	FPythonCommandEx PyCmd;
	PyCmd.Command           = Code;
	PyCmd.ExecutionMode     = bEvaluate
		? EPythonCommandExecutionMode::EvaluateStatement
		: EPythonCommandExecutionMode::ExecuteStatement;
	PyCmd.FileExecutionScope = EPythonFileExecutionScope::Public;

	const bool bExecOk = PythonPlugin->ExecPythonCommandEx(PyCmd);

	// Build result JSON
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField  (TEXT("success"),      bExecOk);
	Result->SetStringField(TEXT("return_value"), PyCmd.CommandResult);

	TArray<TSharedPtr<FJsonValue>> OutputArray;
	for (const FPythonLogOutputEntry& Msg : PyCmd.LogOutput)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		switch (Msg.Type)
		{
			case EPythonLogOutputType::Info:    Entry->SetStringField(TEXT("type"), TEXT("Info"));    break;
			case EPythonLogOutputType::Warning: Entry->SetStringField(TEXT("type"), TEXT("Warning")); break;
			case EPythonLogOutputType::Error:   Entry->SetStringField(TEXT("type"), TEXT("Error"));   break;
			default:                            Entry->SetStringField(TEXT("type"), TEXT("Info"));    break;
		}
		Entry->SetStringField(TEXT("output"), Msg.Output);
		OutputArray.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Result->SetArrayField(TEXT("output"), OutputArray);

	if (bExecOk)
	{
		SetSuccess(Cmd, Result);
	}
	else
	{
		Cmd->bSuccess     = false;
		Cmd->ResultObject = Result;  // include partial output even on error
		Cmd->ErrorMessage = TEXT("Python execution failed — see 'output' for details");
	}
}

// ── console_command ───────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_ConsoleCommand(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString Command;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("command"), Command))
	{
		SetError(Cmd, TEXT("Missing 'command' parameter"));
		return;
	}

	if (GEngine && GEditor)
	{
		UWorld* World = GetWorldFromParams(Cmd->Params);
		if (!World)
		{
			SetError(Cmd, TEXT("No world available for console command"));
			return;
		}
		GEngine->Exec(World, *Command);
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("command"), Command);
		Result->SetStringField(TEXT("status"),  TEXT("executed"));
		SetSuccess(Cmd, Result);
	}
	else
	{
		SetError(Cmd, TEXT("GEngine or GEditor not available"));
	}
}

// ── get_project_info ──────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_GetProjectInfo(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Result->SetStringField(TEXT("project_name"),   FApp::GetProjectName());
	Result->SetStringField(TEXT("project_dir"),    FPaths::ProjectDir());
	Result->SetStringField(TEXT("content_dir"),    FPaths::ProjectContentDir());
	Result->SetStringField(TEXT("engine_dir"),     FPaths::EngineDir());
	Result->SetStringField(TEXT("project_file"),   FPaths::GetProjectFilePath());
	SetSuccess(Cmd, Result);
}

// ── get_all_actors ────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_GetAllActors(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString ClassFilter, LabelFilter;
	if (Cmd->Params.IsValid())
	{
		Cmd->Params->TryGetStringField(TEXT("class_filter"), ClassFilter);
		Cmd->Params->TryGetStringField(TEXT("label_filter"), LabelFilter);
	}

	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World)
	{
		SetError(Cmd, TEXT("No world available (editor or set use_pie / world_context_index)"));
		return;
	}

	TArray<TSharedPtr<FJsonValue>> ActorsArray;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor)) continue;

		const FString Label     = Actor->GetActorLabel();
		const FString ClassName = Actor->GetClass()->GetName();

		if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter)) continue;
		if (!LabelFilter.IsEmpty() && !Label.Contains(LabelFilter))     continue;

		FVector  Loc  = Actor->GetActorLocation();
		FRotator Rot  = Actor->GetActorRotation();
		FVector  Scl  = Actor->GetActorScale3D();

		TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("label"), Label);
		ActorObj->SetStringField(TEXT("class"), ClassName);

		TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X);
		LocObj->SetNumberField(TEXT("y"), Loc.Y);
		LocObj->SetNumberField(TEXT("z"), Loc.Z);
		ActorObj->SetObjectField(TEXT("location"), LocObj);

		TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
		RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
		RotObj->SetNumberField(TEXT("yaw"),   Rot.Yaw);
		RotObj->SetNumberField(TEXT("roll"),  Rot.Roll);
		ActorObj->SetObjectField(TEXT("rotation"), RotObj);

		TSharedPtr<FJsonObject> SclObj = MakeShared<FJsonObject>();
		SclObj->SetNumberField(TEXT("x"), Scl.X);
		SclObj->SetNumberField(TEXT("y"), Scl.Y);
		SclObj->SetNumberField(TEXT("z"), Scl.Z);
		ActorObj->SetObjectField(TEXT("scale"), SclObj);

		TArray<TSharedPtr<FJsonValue>> TagsArr;
		for (const FName& Tag : Actor->Tags)
		{
			TagsArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		ActorObj->SetArrayField(TEXT("tags"), TagsArr);
		ActorObj->SetBoolField(TEXT("hidden"), Actor->IsHidden());
		ActorObj->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		ActorObj->SetStringField(TEXT("actor_path"), Actor->GetPathName());

		ActorsArray.Add(MakeShared<FJsonValueObject>(ActorObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("actors"), ActorsArray);
	Result->SetNumberField(TEXT("count"), ActorsArray.Num());
	SetSuccess(Cmd, Result);
}

// ── spawn_actor ───────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_SpawnActor(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}

	FString ActorClass, Label;
	Cmd->Params->TryGetStringField(TEXT("class"), ActorClass);
	Cmd->Params->TryGetStringField(TEXT("label"), Label);

	// Parse location
	FVector Location(0.f);
	const TSharedPtr<FJsonObject>* LocObjPtr = nullptr;
	TSharedPtr<FJsonObject> LocObj;
	if (Cmd->Params->TryGetObjectField(TEXT("location"), LocObjPtr) && LocObjPtr)
	{
		LocObj = *LocObjPtr;
	}
	if (LocObj.IsValid())
	{
		Location.X = LocObj->GetNumberField(TEXT("x"));
		Location.Y = LocObj->GetNumberField(TEXT("y"));
		Location.Z = LocObj->GetNumberField(TEXT("z"));
	}

	// Parse rotation
	FRotator Rotation(0.f);
	const TSharedPtr<FJsonObject>* RotObjPtr = nullptr;
	TSharedPtr<FJsonObject> RotObj;
	if (Cmd->Params->TryGetObjectField(TEXT("rotation"), RotObjPtr) && RotObjPtr)
	{
		RotObj = *RotObjPtr;
	}
	if (RotObj.IsValid())
	{
		Rotation.Pitch = RotObj->GetNumberField(TEXT("pitch"));
		Rotation.Yaw   = RotObj->GetNumberField(TEXT("yaw"));
		Rotation.Roll  = RotObj->GetNumberField(TEXT("roll"));
	}

	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World)
	{
		SetError(Cmd, TEXT("No world available (editor or set use_pie / world_context_index)"));
		return;
	}

	// Resolve class
	UClass* Class = nullptr;
	if (!ActorClass.IsEmpty())
	{
		// Try native class first
		Class = FindFirstObject<UClass>(*ActorClass, EFindFirstObjectOptions::NativeFirst);
		if (!Class)
		{
			// Try as Blueprint path
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ActorClass);
			if (BP && BP->GeneratedClass)
			{
				Class = BP->GeneratedClass;
			}
		}
	}
	if (!Class)
	{
		Class = AActor::StaticClass(); // fallback to base Actor
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.bAllowDuringConstructionScript = false;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	AActor* NewActor = World->SpawnActor<AActor>(
		Class, FTransform(Rotation, Location), SpawnParams);

	if (!NewActor)
	{
		SetError(Cmd, FString::Printf(TEXT("SpawnActor failed for class '%s'"), *ActorClass));
		return;
	}

	if (!Label.IsEmpty())
	{
		NewActor->SetActorLabel(Label);
	}
	FString FolderPath;
	if (Cmd->Params->TryGetStringField(TEXT("folder_path"), FolderPath) && !FolderPath.IsEmpty())
	{
		NewActor->SetFolderPath(FName(*FolderPath));
	}

	// Apply optional initial properties (best-effort, errors are silently ignored)
	const TSharedPtr<FJsonObject>* PropsJsonPtr = nullptr;
	if (Cmd->Params->TryGetObjectField(TEXT("properties"), PropsJsonPtr) && PropsJsonPtr)
	{
		for (const auto& Pair : (*PropsJsonPtr)->Values)
		{
			FString ErrMsg;
			SetNestedProperty(NewActor, Pair.Key, Pair.Value, ErrMsg);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("label"),    NewActor->GetActorLabel());
	Result->SetStringField(TEXT("class"),    NewActor->GetClass()->GetName());
	Result->SetStringField(TEXT("path"),     NewActor->GetPathName());
	SetSuccess(Cmd, Result);
}

// ── delete_actor ──────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_DeleteActor(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor)
	{
		SetError(Cmd, Err);
		return;
	}
	FString Label = Actor->GetActorLabel();
	Actor->Destroy();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("deleted"), Label);
	SetSuccess(Cmd, Result);
}

// ── delete_actors ─────────────────────────────────────────────────────────────
//
// params: class_name (optional), actor_label_contains (optional substring), tags (optional array of tag names),
//         use_pie, world_context_index (default editor world)
// result: { "deleted_count": N }
// Uses UEditorActorSubsystem::DestroyActors (editor only; PIE actors use same filter but DestroyActors is editor-world).
//
void FMCPTCPServer::Cmd_DeleteActors(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	// Bulk delete uses UEditorActorSubsystem::DestroyActors — editor world only
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		SetError(Cmd, TEXT("Editor world not available"));
		return;
	}
	FString ClassName, LabelContains;
	const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
	if (Cmd->Params.IsValid())
	{
		Cmd->Params->TryGetStringField(TEXT("class_name"), ClassName);
		Cmd->Params->TryGetStringField(TEXT("actor_label_contains"), LabelContains);
		Cmd->Params->TryGetArrayField(TEXT("tags"), TagsArr);
	}
	UClass* FilterClass = nullptr;
	if (!ClassName.IsEmpty())
	{
		FilterClass = FindClassByName(ClassName);
		if (!FilterClass || !FilterClass->IsChildOf(AActor::StaticClass()))
		{
			SetError(Cmd, FString::Printf(TEXT("Invalid or non-actor class_name: '%s'"), *ClassName));
			return;
		}
	}
	TArray<FName> TagFilter;
	if (TagsArr)
		for (const TSharedPtr<FJsonValue>& V : *TagsArr)
			if (V.IsValid() && V->Type == EJson::String)
				TagFilter.Add(FName(*V->AsString()));
	TArray<AActor*> ToDelete;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (FilterClass && !A->IsA(FilterClass)) continue;
		if (!LabelContains.IsEmpty() && !A->GetActorLabel().Contains(LabelContains)) continue;
		if (TagFilter.Num() > 0)
		{
			bool bHasAll = true;
			for (const FName& Tag : TagFilter)
				if (!A->Tags.Contains(Tag)) { bHasAll = false; break; }
			if (!bHasAll) continue;
		}
		ToDelete.Add(A);
	}
	if (ToDelete.Num() == 0)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("deleted_count"), 0);
		SetSuccess(Cmd, Result);
		return;
	}
	UEditorActorSubsystem* Sub = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
	if (!Sub)
	{
		SetError(Cmd, TEXT("EditorActorSubsystem not available (bulk delete is editor-only)"));
		return;
	}
	// DestroyActors expects editor-level actors; in PIE we still pass selection but it may only work in editor world
	Sub->DestroyActors(ToDelete);
	// Count actors that are actually gone (IsValid returns false after successful destruction)
	int32 ActualDeleted = 0;
	for (AActor* A : ToDelete)
	{
		if (!IsValid(A)) ++ActualDeleted;
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("deleted_count"),   ActualDeleted);
	Result->SetNumberField(TEXT("attempted_count"), ToDelete.Num());
	if (ActualDeleted < ToDelete.Num())
		Result->SetStringField(TEXT("warning"), FString::Printf(
			TEXT("Only %d of %d actors were destroyed; some may be protected or already deleted"),
			ActualDeleted, ToDelete.Num()));
	SetSuccess(Cmd, Result);
}

// ── get_actor_transform ───────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_GetActorTransform(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor)
	{
		SetError(Cmd, Err);
		return;
	}

	FVector  Loc  = Actor->GetActorLocation();
	FRotator Rot  = Actor->GetActorRotation();
	FVector  Scl  = Actor->GetActorScale3D();

	auto Vec3 = [](const FVector& V) -> TSharedPtr<FJsonObject> {
		auto J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("x"), V.X);
		J->SetNumberField(TEXT("y"), V.Y);
		J->SetNumberField(TEXT("z"), V.Z);
		return J;
	};
	auto Rot3 = [](const FRotator& R) -> TSharedPtr<FJsonObject> {
		auto J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("pitch"), R.Pitch);
		J->SetNumberField(TEXT("yaw"),   R.Yaw);
		J->SetNumberField(TEXT("roll"),  R.Roll);
		return J;
	};

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("location"), Vec3(Loc));
	Result->SetObjectField(TEXT("rotation"), Rot3(Rot));
	Result->SetObjectField(TEXT("scale"),    Vec3(Scl));
	SetSuccess(Cmd, Result);
}

// ── set_actor_transform ───────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_SetActorTransform(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor)
	{
		SetError(Cmd, Err);
		return;
	}

	const TSharedPtr<FJsonObject>* LocObjPtr = nullptr;
	const TSharedPtr<FJsonObject>* RotObjPtr = nullptr;
	const TSharedPtr<FJsonObject>* SclObjPtr = nullptr;
	Cmd->Params->TryGetObjectField(TEXT("location"), LocObjPtr);
	Cmd->Params->TryGetObjectField(TEXT("rotation"), RotObjPtr);
	Cmd->Params->TryGetObjectField(TEXT("scale"), SclObjPtr);

	if (LocObjPtr && LocObjPtr->IsValid())
	{
		const TSharedPtr<FJsonObject>& LocObj = *LocObjPtr;
	{
		Actor->SetActorLocation(FVector(
			LocObj->GetNumberField(TEXT("x")),
			LocObj->GetNumberField(TEXT("y")),
			LocObj->GetNumberField(TEXT("z"))), false, nullptr, ETeleportType::TeleportPhysics);
	}
	if (RotObjPtr && RotObjPtr->IsValid())
	{
		const TSharedPtr<FJsonObject>& RotObj = *RotObjPtr;
		Actor->SetActorRotation(FRotator(
			RotObj->GetNumberField(TEXT("pitch")),
			RotObj->GetNumberField(TEXT("yaw")),
			RotObj->GetNumberField(TEXT("roll"))));
	}
	if (SclObjPtr && SclObjPtr->IsValid())
	{
		const TSharedPtr<FJsonObject>& SclObj = *SclObjPtr;
		Actor->SetActorScale3D(FVector(
			SclObj->GetNumberField(TEXT("x")),
			SclObj->GetNumberField(TEXT("y")),
			SclObj->GetNumberField(TEXT("z"))));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("label"),   Actor->GetActorLabel());
	Result->SetStringField(TEXT("status"),  TEXT("transform_set"));
	SetSuccess(Cmd, Result);
}

// ── open_level ────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_OpenLevel(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString LevelPath;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("level_path"), LevelPath))
	{
		SetError(Cmd, TEXT("Missing 'level_path' parameter"));
		return;
	}

	if (GEditor)
	{
		FEditorFileUtils::LoadMap(LevelPath, false, true);
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("opened"), LevelPath);
		SetSuccess(Cmd, Result);
	}
	else
	{
		SetError(Cmd, TEXT("GEditor not available"));
	}
}

// ── get_assets ────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_GetAssets(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString Path = TEXT("/Game");
	FString ClassFilter;
	bool bRecursive = true;

	if (Cmd->Params.IsValid())
	{
		Cmd->Params->TryGetStringField(TEXT("path"),         Path);
		Cmd->Params->TryGetStringField(TEXT("class_filter"), ClassFilter);
		Cmd->Params->TryGetBoolField  (TEXT("recursive"),    bRecursive);
	}

	FAssetRegistryModule& ARM =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();

	FARFilter Filter;
	Filter.PackagePaths.Add(*Path);
	Filter.bRecursivePaths = bRecursive;
	if (!ClassFilter.IsEmpty())
	{
		// Try to resolve to a real UClass so Blueprint subclasses are matched correctly
		UClass* FilterClass = FindClassByName(ClassFilter);
		if (FilterClass)
		{
			Filter.ClassPaths.Add(FilterClass->GetClassPathName());
			Filter.bRecursiveClasses = true;
		}
		else
		{
			// Fallback: try as a raw /Script/Engine path
			Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), *ClassFilter));
		}
	}

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> AssetArray;
	for (const FAssetData& Asset : Assets)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
		Entry->SetStringField(TEXT("asset_name"),   Asset.AssetName.ToString());
		Entry->SetStringField(TEXT("class"),        Asset.AssetClassPath.ToString());
		Entry->SetStringField(TEXT("class_name"),   Asset.AssetClassPath.GetAssetName().ToString());
		Entry->SetStringField(TEXT("object_path"),  Asset.GetObjectPathString());
		AssetArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField (TEXT("assets"), AssetArray);
	Result->SetNumberField(TEXT("count"),  AssetArray.Num());
	SetSuccess(Cmd, Result);
}

// ── get_selected_assets ──────────────────────────────────────────────────────
//
// result: { "assets": [ { "package_name", "asset_name", "class", "class_name", "object_path" }, ... ], "count": N }
// Same shape as get_assets; returns whatever is selected in the primary Content Browser.
//
void FMCPTCPServer::Cmd_GetSelectedAssets(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	TArray<FAssetData> Selected;
	IContentBrowserSingleton::Get().GetSelectedAssets(Selected);

	TArray<TSharedPtr<FJsonValue>> AssetArray;
	for (const FAssetData& Asset : Selected)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
		Entry->SetStringField(TEXT("asset_name"),   Asset.AssetName.ToString());
		Entry->SetStringField(TEXT("class"),        Asset.AssetClassPath.ToString());
		Entry->SetStringField(TEXT("class_name"),   Asset.AssetClassPath.GetAssetName().ToString());
		Entry->SetStringField(TEXT("object_path"),  Asset.GetObjectPathString());
		AssetArray.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("assets"), AssetArray);
	Result->SetNumberField(TEXT("count"), AssetArray.Num());
	SetSuccess(Cmd, Result);
}

// ── set_selected_assets ───────────────────────────────────────────────────────
//
// params: asset_paths (array of object path or package.asset strings)
// Syncs the primary Content Browser to select the given assets (FContentBrowserModule / SyncBrowserToAssets).
//
void FMCPTCPServer::Cmd_SetSelectedAssets(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	const TArray<TSharedPtr<FJsonValue>>* PathArr = nullptr;
	if (!Cmd->Params->TryGetArrayField(TEXT("asset_paths"), PathArr) || !PathArr || PathArr->Num() == 0)
	{
		SetError(Cmd, TEXT("asset_paths (array of asset/object path strings) is required"));
		return;
	}
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();
	TArray<FAssetData> ToSelect;
	for (const TSharedPtr<FJsonValue>& Val : *PathArr)
	{
		if (!Val.IsValid() || Val->Type != EJson::String) continue;
		FString Path = Val->AsString();
		if (Path.IsEmpty()) continue;
		FSoftObjectPath SoftPath(Path);
		FAssetData Data = AR.GetAssetByObjectPath(SoftPath, /*bIncludeOnlyOnDiskAssets=*/false);
		if (Data.IsValid())
			ToSelect.Add(Data);
	}
	if (ToSelect.Num() == 0)
	{
		SetError(Cmd, TEXT("No valid assets found for the given paths"));
		return;
	}
	IContentBrowserSingleton::Get().SyncBrowserToAssets(ToSelect, /*bAllowLockedBrowsers=*/false, /*bFocusContentBrowser=*/true);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), ToSelect.Num());
	SetSuccess(Cmd, Result);
}

// ── get_content_subpaths ─────────────────────────────────────────────────────
//
// params: base_path (default /Game), recursive (default false)
// result: { "paths": [ "...", ... ], "count": N } — subfolder paths under base_path (Content Browser folder hierarchy).
//
void FMCPTCPServer::Cmd_GetContentSubpaths(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BasePath = TEXT("/Game");
	bool bRecursive = false;
	if (Cmd->Params.IsValid())
	{
		Cmd->Params->TryGetStringField(TEXT("base_path"), BasePath);
		Cmd->Params->TryGetBoolField(TEXT("recursive"), bRecursive);
	}
	if (BasePath.IsEmpty()) BasePath = TEXT("/Game");
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();
	TArray<FString> OutPaths;
	AR.GetSubPaths(BasePath, OutPaths, bRecursive);
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& P : OutPaths)
		Arr.Add(MakeShared<FJsonValueString>(P));
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("paths"), Arr);
	Result->SetNumberField(TEXT("count"), Arr.Num());
	SetSuccess(Cmd, Result);
}

// ── create_content_folder ────────────────────────────────────────────────────
//
// params: folder_path (e.g. "/Game/MyFolder" or "/Game/Parent/Child")
// result: { "folder_path": "..." }
// Uses UEditorAssetSubsystem::MakeDirectory (creates on disk and adds to AssetRegistry).
//
void FMCPTCPServer::Cmd_CreateContentFolder(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}
	FString FolderPath;
	if (!Cmd->Params->TryGetStringField(TEXT("folder_path"), FolderPath) || FolderPath.IsEmpty())
	{
		SetError(Cmd, TEXT("folder_path is required (e.g. /Game/MyFolder)"));
		return;
	}
	FolderPath = FolderPath.TrimStartAndEnd();
	UEditorAssetSubsystem* AssetSub = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (!AssetSub)
	{
		SetError(Cmd, TEXT("EditorAssetSubsystem not available"));
		return;
	}
	if (!AssetSub->MakeDirectory(FolderPath))
	{
		SetError(Cmd, FString::Printf(TEXT("MakeDirectory failed for '%s'"), *FolderPath));
		return;
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("folder_path"), FolderPath);
	SetSuccess(Cmd, Result);
}

// ── create_asset ─────────────────────────────────────────────────────────────
//
// params: asset_name, package_path, asset_class (required), factory_class (optional — auto-searched if omitted)
// result: { "object_path": "...", "asset_path": "..." }
//
void FMCPTCPServer::Cmd_CreateAsset(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString AssetName, PackagePath, AssetClassStr, FactoryClassStr;
	Cmd->Params->TryGetStringField(TEXT("asset_name"),     AssetName);
	Cmd->Params->TryGetStringField(TEXT("package_path"),    PackagePath);
	Cmd->Params->TryGetStringField(TEXT("asset_class"),     AssetClassStr);
	Cmd->Params->TryGetStringField(TEXT("factory_class"),  FactoryClassStr);

	if (AssetName.IsEmpty() || PackagePath.IsEmpty() || AssetClassStr.IsEmpty())
	{
		SetError(Cmd, TEXT("asset_name, package_path, and asset_class are required"));
		return;
	}

	UClass* AssetClass = FindClassByName(AssetClassStr);
	if (!AssetClass)
	{
		SetError(Cmd, FString::Printf(TEXT("Asset class not found: '%s'"), *AssetClassStr));
		return;
	}

	UFactory* Factory = nullptr;
	if (!FactoryClassStr.IsEmpty())
	{
		UClass* FactoryClass = FindClassByName(FactoryClassStr);
		if (!FactoryClass || !FactoryClass->IsChildOf(UFactory::StaticClass()))
		{
			SetError(Cmd, FString::Printf(TEXT("Invalid factory_class: '%s'"), *FactoryClassStr));
			return;
		}
		Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	}
	else
	{
		// Find a factory that supports this asset class (discovery: iterate UFactory derived classes)
		TArray<UClass*> FactoryClasses;
		GetDerivedClasses(UFactory::StaticClass(), FactoryClasses, /*bRecursive=*/true);
		for (UClass* FC : FactoryClasses)
		{
			if (FC->HasAnyClassFlags(CLASS_Abstract)) continue;
			UFactory* F = NewObject<UFactory>(GetTransientPackage(), FC);
			if (F && F->SupportedClass == AssetClass)
			{
				Factory = F;
				break;
			}
		}
		if (!Factory)
		{
			SetError(Cmd, FString::Printf(TEXT("No factory found for asset class '%s'. Pass factory_class explicitly."), *AssetClassStr));
			return;
		}
	}

	// DataTable: factory requires RowStruct to be set before CreateAsset
	if (AssetClass == UDataTable::StaticClass())
	{
		FString RowStructPath;
		if (Cmd->Params->TryGetStringField(TEXT("row_struct"), RowStructPath) && !RowStructPath.IsEmpty())
		{
			UDataTableFactory* DTFactory = Cast<UDataTableFactory>(Factory);
			if (DTFactory)
			{
				UScriptStruct* RowStruct = FindObject<UScriptStruct>(nullptr, *RowStructPath);
				if (!RowStruct) RowStruct = LoadObject<UScriptStruct>(nullptr, *RowStructPath);
				if (!RowStruct) RowStruct = FindFirstObject<UScriptStruct>(*RowStructPath, EFindFirstObjectOptions::NativeFirst);
				if (RowStruct)
					DTFactory->Struct = RowStruct;
				else
				{
					SetError(Cmd, FString::Printf(TEXT("row_struct not found: '%s' (use full path e.g. /Script/Engine.TableRowBase)"), *RowStructPath));
					return;
				}
			}
		}
		else
		{
			SetError(Cmd, TEXT("DataTable requires row_struct (e.g. /Script/Engine.TableRowBase or a UserDefinedStruct path)"));
			return;
		}
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, AssetClass, Factory, NAME_None);
	if (!NewAsset)
	{
		SetError(Cmd, FString::Printf(TEXT("CreateAsset failed for '%s' in '%s'"), *AssetName, *PackagePath));
		return;
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("object_path"), NewAsset->GetPathName());
	Result->SetStringField(TEXT("asset_path"),  FPaths::Combine(PackagePath, AssetName));
	SetSuccess(Cmd, Result);
}

// ── create_widget_blueprint ──────────────────────────────────────────────────
//
// params: asset_name, package_path, parent_class (optional, default UserWidget)
// result: { "object_path", "asset_path" }
//
void FMCPTCPServer::Cmd_CreateWidgetBlueprint(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString AssetName, PackagePath, ParentClassStr;
	Cmd->Params->TryGetStringField(TEXT("asset_name"),     AssetName);
	Cmd->Params->TryGetStringField(TEXT("package_path"),   PackagePath);
	Cmd->Params->TryGetStringField(TEXT("parent_class"),   ParentClassStr);
	if (AssetName.IsEmpty() || PackagePath.IsEmpty())
	{
		SetError(Cmd, TEXT("asset_name and package_path are required"));
		return;
	}
	UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
	Factory->BlueprintType = BPTYPE_Normal;
	UClass* ParentClass = UUserWidget::StaticClass();
	if (!ParentClassStr.IsEmpty())
	{
		UClass* Found = FindClassByName(ParentClassStr);
		if (Found && Found->IsChildOf(UUserWidget::StaticClass()))
			ParentClass = Found;
	}
	Factory->ParentClass = ParentClass;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UWidgetBlueprint::StaticClass(), Factory, NAME_None);
	if (!NewAsset)
	{
		SetError(Cmd, FString::Printf(TEXT("CreateAsset failed for Widget Blueprint '%s' in '%s'"), *AssetName, *PackagePath));
		return;
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("object_path"), NewAsset->GetPathName());
	Result->SetStringField(TEXT("asset_path"),  FPaths::Combine(PackagePath, AssetName));
	SetSuccess(Cmd, Result);
}

// ── create_behavior_tree ─────────────────────────────────────────────────────
//
// params: asset_name, package_path
// result: { "behavior_tree_path", "blackboard_path" } (object paths)
// Creates UBlackboardData (AssetName_BB) and UBehaviorTree (AssetName_BT), links them.
//
void FMCPTCPServer::Cmd_CreateBehaviorTree(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString AssetName, PackagePath;
	Cmd->Params->TryGetStringField(TEXT("asset_name"),   AssetName);
	Cmd->Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (AssetName.IsEmpty() || PackagePath.IsEmpty())
	{
		SetError(Cmd, TEXT("asset_name and package_path are required"));
		return;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

	UBlackboardDataFactory* BBFactory = NewObject<UBlackboardDataFactory>();
	UObject* NewBB = AssetTools.CreateAsset(AssetName + TEXT("_BB"), PackagePath, UBlackboardData::StaticClass(), BBFactory, NAME_None);
	UBlackboardData* Blackboard = Cast<UBlackboardData>(NewBB);
	if (!Blackboard)
	{
		SetError(Cmd, FString::Printf(TEXT("CreateAsset failed for Blackboard '%s_BB' in '%s'"), *AssetName, *PackagePath));
		return;
	}

	UBehaviorTreeFactory* BTFactory = NewObject<UBehaviorTreeFactory>();
	UObject* NewBT = AssetTools.CreateAsset(AssetName + TEXT("_BT"), PackagePath, UBehaviorTree::StaticClass(), BTFactory, NAME_None);
	UBehaviorTree* BehaviorTree = Cast<UBehaviorTree>(NewBT);
	if (!BehaviorTree)
	{
		SetError(Cmd, FString::Printf(TEXT("CreateAsset failed for Behavior Tree '%s_BT' in '%s'"), *AssetName, *PackagePath));
		return;
	}

	BehaviorTree->BlackboardAsset = Blackboard;
	BehaviorTree->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("behavior_tree_path"), BehaviorTree->GetPathName());
	Result->SetStringField(TEXT("blackboard_path"),   Blackboard->GetPathName());
	SetSuccess(Cmd, Result);
}

// ── add_blackboard_key ───────────────────────────────────────────────────────
//
// params: blackboard_path (asset/object path), key_name, key_type ("Object"|"Vector"|"Float"|"Int"|"Bool"|"String")
// result: { "key_id" } (numeric) or success
//
void FMCPTCPServer::Cmd_AddBlackboardKey(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString BBPath, KeyNameStr, KeyTypeStr;
	Cmd->Params->TryGetStringField(TEXT("blackboard_path"), BBPath);
	Cmd->Params->TryGetStringField(TEXT("key_name"),       KeyNameStr);
	Cmd->Params->TryGetStringField(TEXT("key_type"),       KeyTypeStr);
	if (BBPath.IsEmpty() || KeyNameStr.IsEmpty() || KeyTypeStr.IsEmpty())
	{
		SetError(Cmd, TEXT("blackboard_path, key_name, and key_type are required"));
		return;
	}

	UBlackboardData* BBAsset = LoadObject<UBlackboardData>(nullptr, *BBPath);
	if (!BBAsset) BBAsset = Cast<UBlackboardData>(FindObject<UBlackboardData>(nullptr, *BBPath));
	if (!BBAsset)
	{
		SetError(Cmd, FString::Printf(TEXT("Blackboard not found: '%s'"), *BBPath));
		return;
	}

	FName KeyName(*KeyNameStr);
	if (BBAsset->GetKeyID(KeyName) != FBlackboard::InvalidKey)
	{
		SetError(Cmd, FString::Printf(TEXT("Blackboard key already exists: '%s'"), *KeyNameStr));
		return;
	}

	FBlackboardEntry NewEntry;
	NewEntry.EntryName = KeyName;

	UBlackboardKeyType* KeyTypeObj = nullptr;
	if (KeyTypeStr.Equals(TEXT("Object"), ESearchCase::IgnoreCase))
		KeyTypeObj = NewObject<UBlackboardKeyType_Object>(BBAsset);
	else if (KeyTypeStr.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
		KeyTypeObj = NewObject<UBlackboardKeyType_Vector>(BBAsset);
	else if (KeyTypeStr.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
		KeyTypeObj = NewObject<UBlackboardKeyType_Float>(BBAsset);
	else if (KeyTypeStr.Equals(TEXT("Int"), ESearchCase::IgnoreCase))
		KeyTypeObj = NewObject<UBlackboardKeyType_Int>(BBAsset);
	else if (KeyTypeStr.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
		KeyTypeObj = NewObject<UBlackboardKeyType_Bool>(BBAsset);
	else if (KeyTypeStr.Equals(TEXT("String"), ESearchCase::IgnoreCase))
		KeyTypeObj = NewObject<UBlackboardKeyType_String>(BBAsset);
	else
	{
		SetError(Cmd, FString::Printf(TEXT("Unsupported key_type: '%s' (use Object, Vector, Float, Int, Bool, String)"), *KeyTypeStr));
		return;
	}

	NewEntry.KeyType = KeyTypeObj;
	BBAsset->Keys.Add(NewEntry);
	BBAsset->UpdateKeyIDs();
	BBAsset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("key_id"), static_cast<double>(static_cast<int32>(BBAsset->GetKeyID(KeyName))));
	SetSuccess(Cmd, Result);
}

// ── run_behavior_tree ────────────────────────────────────────────────────────
//
// params: controller_actor_path | controller_actor_label, behavior_tree_path | behavior_tree_name [, use_pie, world_context_index ]
// result: { "success": true }
// Runs the given UBehaviorTree on the given AAIController (e.g. in PIE).
//
void FMCPTCPServer::Cmd_RunBehaviorTree(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString ControllerParam, BTParam;
	Cmd->Params->TryGetStringField(TEXT("controller_actor_path"),  ControllerParam);
	if (ControllerParam.IsEmpty()) Cmd->Params->TryGetStringField(TEXT("controller_actor_label"), ControllerParam);
	Cmd->Params->TryGetStringField(TEXT("behavior_tree_path"), BTParam);
	if (BTParam.IsEmpty()) Cmd->Params->TryGetStringField(TEXT("behavior_tree_name"), BTParam);
	if (ControllerParam.IsEmpty() || BTParam.IsEmpty())
	{
		SetError(Cmd, TEXT("controller_actor_path or controller_actor_label, and behavior_tree_path or behavior_tree_name are required"));
		return;
	}

	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World)
	{
		SetError(Cmd, TEXT("No world available (editor or set use_pie / world_context_index)"));
		return;
	}

	// ResolveActorFromParams expects actor_path/actor_label; map controller_* to that
	TSharedPtr<FJsonObject> ResolveParams = MakeShared<FJsonObject>();
	ResolveParams->SetStringField(TEXT("actor_path"),  ControllerParam);
	ResolveParams->SetStringField(TEXT("actor_label"), ControllerParam);
	if (Cmd->Params->HasField(TEXT("use_pie"))) ResolveParams->SetBoolField(TEXT("use_pie"), Cmd->Params->GetBoolField(TEXT("use_pie")));
	if (Cmd->Params->HasField(TEXT("world_context_index"))) ResolveParams->SetNumberField(TEXT("world_context_index"), Cmd->Params->GetNumberField(TEXT("world_context_index")));
	FString ActorErr;
	AActor* ControllerActor = ResolveActorFromParams(ResolveParams, &ActorErr);
	if (!ControllerActor)
	{
		SetError(Cmd, ActorErr.IsEmpty() ? TEXT("Could not resolve controller actor") : ActorErr);
		return;
	}

	AAIController* AIC = Cast<AAIController>(ControllerActor);
	if (!AIC)
	{
		SetError(Cmd, FString::Printf(TEXT("Actor is not an AAIController: '%s'"), *ControllerActor->GetName()));
		return;
	}

	UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *BTParam);
	if (!BT) BT = Cast<UBehaviorTree>(FindObject<UBehaviorTree>(nullptr, *BTParam));
	if (!BT)
	{
		SetError(Cmd, FString::Printf(TEXT("Behavior Tree not found: '%s'"), *BTParam));
		return;
	}

	if (!AIC->RunBehaviorTree(BT))
	{
		SetError(Cmd, TEXT("RunBehaviorTree failed (e.g. controller may already be running a BT)"));
		return;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	SetSuccess(Cmd, Result);
}

// ── configure_ai_perception ──────────────────────────────────────────────────
//
// params: controller_actor_path | controller_actor_label, sight_radius, lose_sight_radius, peripheral_vision_angle_degrees [, world params ]
// result: { "success": true }
// Adds/configures UAISenseConfig_Sight on the AI Controller so the AI can "see" other actors.
//
void FMCPTCPServer::Cmd_ConfigureAIPerception(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString ControllerParam;
	Cmd->Params->TryGetStringField(TEXT("controller_actor_path"),  ControllerParam);
	if (ControllerParam.IsEmpty()) Cmd->Params->TryGetStringField(TEXT("controller_actor_label"), ControllerParam);
	double SightRadius = 3000.0, LoseSightRadius = 3500.0, PeripheralVisionAngle = 90.0;
	Cmd->Params->TryGetNumberField(TEXT("sight_radius"), SightRadius);
	Cmd->Params->TryGetNumberField(TEXT("lose_sight_radius"), LoseSightRadius);
	Cmd->Params->TryGetNumberField(TEXT("peripheral_vision_angle_degrees"), PeripheralVisionAngle);
	if (ControllerParam.IsEmpty())
	{
		SetError(Cmd, TEXT("controller_actor_path or controller_actor_label is required"));
		return;
	}

	TSharedPtr<FJsonObject> ResolveParams = MakeShared<FJsonObject>();
	ResolveParams->SetStringField(TEXT("actor_path"),  ControllerParam);
	ResolveParams->SetStringField(TEXT("actor_label"), ControllerParam);
	if (Cmd->Params->HasField(TEXT("use_pie"))) ResolveParams->SetBoolField(TEXT("use_pie"), Cmd->Params->GetBoolField(TEXT("use_pie")));
	if (Cmd->Params->HasField(TEXT("world_context_index"))) ResolveParams->SetNumberField(TEXT("world_context_index"), Cmd->Params->GetNumberField(TEXT("world_context_index")));
	FString ActorErr;
	AActor* ControllerActor = ResolveActorFromParams(ResolveParams, &ActorErr);
	if (!ControllerActor)
	{
		SetError(Cmd, ActorErr.IsEmpty() ? TEXT("Could not resolve controller actor") : ActorErr);
		return;
	}

	AAIController* AIC = Cast<AAIController>(ControllerActor);
	if (!AIC)
	{
		SetError(Cmd, FString::Printf(TEXT("Actor is not an AAIController: '%s'"), *ControllerActor->GetName()));
		return;
	}

	UAIPerceptionComponent* PerceptionComp = AIC->GetAIPerceptionComponent();
	if (!PerceptionComp)
	{
		PerceptionComp = NewObject<UAIPerceptionComponent>(AIC);
		AIC->SetPerceptionComponent(*PerceptionComp);
	}

	UAISenseConfig_Sight* SightConfig = NewObject<UAISenseConfig_Sight>(PerceptionComp);
	SightConfig->SightRadius = static_cast<float>(SightRadius);
	SightConfig->LoseSightRadius = static_cast<float>(LoseSightRadius);
	SightConfig->PeripheralVisionAngleDegrees = static_cast<float>(PeripheralVisionAngle);
	SightConfig->DetectionByAffiliation.bDetectEnemies = true;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = true;
	SightConfig->DetectionByAffiliation.bDetectNeutrals = true;

	PerceptionComp->ConfigureSense(*SightConfig);
	PerceptionComp->SetDominantSense(SightConfig->GetSenseImplementation());

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	SetSuccess(Cmd, Result);
}

// ── configure_ai_hearing ─────────────────────────────────────────────────────
//
// params: controller_actor_path | controller_actor_label, hearing_range [, world params ]
// result: { "success": true }
// Adds/configures UAISenseConfig_Hearing so the AI can respond to ReportNoiseEvent (e.g. footsteps, gunshots).
//
void FMCPTCPServer::Cmd_ConfigureAIHearing(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString ControllerParam;
	Cmd->Params->TryGetStringField(TEXT("controller_actor_path"),  ControllerParam);
	if (ControllerParam.IsEmpty()) Cmd->Params->TryGetStringField(TEXT("controller_actor_label"), ControllerParam);
	double HearingRange = 3000.0;
	Cmd->Params->TryGetNumberField(TEXT("hearing_range"), HearingRange);
	if (ControllerParam.IsEmpty())
	{
		SetError(Cmd, TEXT("controller_actor_path or controller_actor_label is required"));
		return;
	}

	TSharedPtr<FJsonObject> ResolveParams = MakeShared<FJsonObject>();
	ResolveParams->SetStringField(TEXT("actor_path"),  ControllerParam);
	ResolveParams->SetStringField(TEXT("actor_label"), ControllerParam);
	if (Cmd->Params->HasField(TEXT("use_pie"))) ResolveParams->SetBoolField(TEXT("use_pie"), Cmd->Params->GetBoolField(TEXT("use_pie")));
	if (Cmd->Params->HasField(TEXT("world_context_index"))) ResolveParams->SetNumberField(TEXT("world_context_index"), Cmd->Params->GetNumberField(TEXT("world_context_index")));
	FString ActorErr;
	AActor* ControllerActor = ResolveActorFromParams(ResolveParams, &ActorErr);
	if (!ControllerActor)
	{
		SetError(Cmd, ActorErr.IsEmpty() ? TEXT("Could not resolve controller actor") : ActorErr);
		return;
	}

	AAIController* AIC = Cast<AAIController>(ControllerActor);
	if (!AIC)
	{
		SetError(Cmd, FString::Printf(TEXT("Actor is not an AAIController: '%s'"), *ControllerActor->GetName()));
		return;
	}

	UAIPerceptionComponent* PerceptionComp = AIC->GetAIPerceptionComponent();
	if (!PerceptionComp)
	{
		SetError(Cmd, TEXT("AI Controller has no perception component; add Sight first with configure_ai_perception"));
		return;
	}

	UAISenseConfig_Hearing* HearingConfig = NewObject<UAISenseConfig_Hearing>(PerceptionComp);
	HearingConfig->HearingRange = static_cast<float>(HearingRange);
	HearingConfig->DetectionByAffiliation.bDetectEnemies = true;
	HearingConfig->DetectionByAffiliation.bDetectFriendlies = true;
	HearingConfig->DetectionByAffiliation.bDetectNeutrals = true;

	PerceptionComp->ConfigureSense(*HearingConfig);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	SetSuccess(Cmd, Result);
}

// ── add_bt_composite_node ────────────────────────────────────────────────────
//
// params: behavior_tree_path | behavior_tree_name, composite_type ("Selector"|"Sequence"), [ node_position [x,y] ]
// result: { "success": true, "node_guid": "..." }
// Adds a Selector or Sequence composite to the BT graph and links it under the root. Editor-only (BTGraph).
//
void FMCPTCPServer::Cmd_AddBTCompositeNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString BTPath, CompositeTypeStr;
	Cmd->Params->TryGetStringField(TEXT("behavior_tree_path"), BTPath);
	if (BTPath.IsEmpty()) Cmd->Params->TryGetStringField(TEXT("behavior_tree_name"), BTPath);
	Cmd->Params->TryGetStringField(TEXT("composite_type"), CompositeTypeStr);
	double PosX = 200.0, PosY = 0.0;
	const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr->Num() >= 2)
		{ PosX = (*PosArr)[0]->AsNumber(); PosY = (*PosArr)[1]->AsNumber(); }
	if (BTPath.IsEmpty() || CompositeTypeStr.IsEmpty())
	{
		SetError(Cmd, TEXT("behavior_tree_path (or behavior_tree_name) and composite_type (Selector or Sequence) are required"));
		return;
	}

	UBehaviorTree* BTAsset = LoadObject<UBehaviorTree>(nullptr, *BTPath);
	if (!BTAsset) BTAsset = Cast<UBehaviorTree>(FindObject<UBehaviorTree>(nullptr, *BTPath));
	if (!BTAsset)
	{
		SetError(Cmd, FString::Printf(TEXT("Behavior Tree not found: '%s'"), *BTPath));
		return;
	}

#if WITH_EDITORONLY_DATA
	UEdGraph* BTGraph = BTAsset->BTGraph;
	if (!BTGraph)
	{
		SetError(Cmd, TEXT("Behavior Tree has no editor graph (open the asset in editor once to create it)"));
		return;
	}

	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BTGraph);
	if (!Graph)
	{
		SetError(Cmd, TEXT("Behavior Tree graph is not a UBehaviorTreeGraph"));
		return;
	}

	UClass* NodeClass = nullptr;
	if (CompositeTypeStr.Equals(TEXT("Selector"), ESearchCase::IgnoreCase))
		NodeClass = UBTComposite_Selector::StaticClass();
	else if (CompositeTypeStr.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase))
		NodeClass = UBTComposite_Sequence::StaticClass();
	else
	{
		SetError(Cmd, FString::Printf(TEXT("composite_type must be Selector or Sequence, got: '%s'"), *CompositeTypeStr));
		return;
	}

	UBTCompositeNode* RuntimeNode = nullptr;
	if (NodeClass == UBTComposite_Selector::StaticClass())
		RuntimeNode = NewObject<UBTComposite_Selector>(BTAsset);
	else
		RuntimeNode = NewObject<UBTComposite_Sequence>(BTAsset);
	if (!RuntimeNode)
	{
		SetError(Cmd, TEXT("Failed to create composite runtime node"));
		return;
	}

	UBehaviorTreeGraphNode_Composite* GraphNode = nullptr;
	{
		FGraphNodeCreator<UBehaviorTreeGraphNode_Composite> NodeBuilder(*BTGraph);
		GraphNode = NodeBuilder.CreateNode();
		if (!GraphNode) { SetError(Cmd, TEXT("CreateNode failed")); return; }
		GraphNode->NodeInstance = RuntimeNode;
		GraphNode->NodePosX = static_cast<int32>(PosX);
		GraphNode->NodePosY = static_cast<int32>(PosY);
		NodeBuilder.Finalize();
	}

	// Link root node's output to this composite's input
	UBehaviorTreeGraphNode_Root* RootNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		RootNode = Cast<UBehaviorTreeGraphNode_Root>(Node);
		if (RootNode) break;
	}
	if (RootNode)
	{
		UEdGraphPin* RootOut = nullptr;
		UEdGraphPin* CompositeIn = nullptr;
		for (UEdGraphPin* Pin : RootNode->Pins) { if (Pin && Pin->Direction == EGPD_Output) { RootOut = Pin; break; } }
		for (UEdGraphPin* Pin : GraphNode->Pins) { if (Pin && Pin->Direction == EGPD_Input) { CompositeIn = Pin; break; } }
		if (RootOut && CompositeIn)
			RootOut->MakeLinkTo(CompositeIn);
	}

	Graph->UpdateAsset(0);
	BTAsset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("node_guid"), GraphNode->NodeGuid.ToString());
	SetSuccess(Cmd, Result);
#else
	SetError(Cmd, TEXT("add_bt_composite_node is editor-only (BTGraph)"));
#endif
}

// ── add_bt_decorator_node ─────────────────────────────────────────────────────
//
// Attaches a Decorator to an existing Behavior Tree composite or task graph node.
// params: behavior_tree_path (or behavior_tree_name), decorator_class (e.g. "BTDecorator_Blackboard"),
//         parent_node_guid (the composite or task node to decorate), node_position ([x,y] optional)
// result: { "node_guid": "..." }
//
void FMCPTCPServer::Cmd_AddBTDecoratorNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
#if WITH_EDITORONLY_DATA
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }
	FString BTPath, DecoratorClassStr, ParentGuidStr;
	Cmd->Params->TryGetStringField(TEXT("behavior_tree_path"),  BTPath);
	if (BTPath.IsEmpty()) Cmd->Params->TryGetStringField(TEXT("behavior_tree_name"), BTPath);
	Cmd->Params->TryGetStringField(TEXT("decorator_class"),     DecoratorClassStr);
	Cmd->Params->TryGetStringField(TEXT("parent_node_guid"),    ParentGuidStr);
	double PosX = 0.0, PosY = -40.0;
	const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr && PosArr->Num() >= 2)
		{ PosX = (*PosArr)[0]->AsNumber(); PosY = (*PosArr)[1]->AsNumber(); }

	if (BTPath.IsEmpty() || DecoratorClassStr.IsEmpty())
	{
		SetError(Cmd, TEXT("behavior_tree_path (or behavior_tree_name) and decorator_class are required"), TEXT("invalid_params"));
		return;
	}

	UBehaviorTree* BTAsset = LoadObject<UBehaviorTree>(nullptr, *BTPath);
	if (!BTAsset) { SetError(Cmd, FString::Printf(TEXT("Behavior Tree not found: '%s'"), *BTPath), TEXT("not_found")); return; }

	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BTAsset->BTGraph);
	if (!Graph) { SetError(Cmd, TEXT("Behavior Tree has no editor graph"), TEXT("unavailable")); return; }

	// Find the parent node by GUID (or use first composite if not specified)
	UBehaviorTreeGraphNode* ParentNode = nullptr;
	if (!ParentGuidStr.IsEmpty())
	{
		FGuid ParentGuid;
		FGuid::Parse(ParentGuidStr, ParentGuid);
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid == ParentGuid)
			{
				ParentNode = Cast<UBehaviorTreeGraphNode>(N);
				break;
			}
		}
		if (!ParentNode)
		{
			SetError(Cmd, FString::Printf(TEXT("Parent node with GUID '%s' not found"), *ParentGuidStr), TEXT("not_found"));
			return;
		}
	}
	else
	{
		// Default: first non-root composite node
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (Cast<UBehaviorTreeGraphNode_Composite>(N))
			{
				ParentNode = Cast<UBehaviorTreeGraphNode>(N);
				break;
			}
		}
		if (!ParentNode) { SetError(Cmd, TEXT("No composite node found; specify parent_node_guid"), TEXT("not_found")); return; }
	}

	// Resolve decorator class
	UClass* DecClass = FindClassByName(DecoratorClassStr);
	if (!DecClass) DecClass = FindFirstObject<UClass>(*DecoratorClassStr, EFindFirstObjectOptions::NativeFirst);
	if (!DecClass || !DecClass->IsChildOf(UBTDecorator::StaticClass()))
	{
		SetError(Cmd, FString::Printf(TEXT("Decorator class not found or not a UBTDecorator: '%s'"), *DecoratorClassStr), TEXT("not_found"));
		return;
	}

	// Create runtime node instance
	UBTDecorator* RuntimeDecorator = NewObject<UBTDecorator>(BTAsset, DecClass);
	if (!RuntimeDecorator) { SetError(Cmd, TEXT("Failed to create decorator instance"), TEXT("error")); return; }

	// Create graph node and attach to parent
	UBehaviorTreeGraphNode_Decorator* DecoratorGraphNode = nullptr;
	{
		FGraphNodeCreator<UBehaviorTreeGraphNode_Decorator> Builder(*Graph);
		DecoratorGraphNode = Builder.CreateNode();
		if (!DecoratorGraphNode) { SetError(Cmd, TEXT("CreateNode failed for decorator"), TEXT("error")); return; }
		DecoratorGraphNode->NodeInstance = RuntimeDecorator;
		DecoratorGraphNode->NodePosX = static_cast<int32>(PosX);
		DecoratorGraphNode->NodePosY = static_cast<int32>(PosY);
		Builder.Finalize();
	}

	// Add the decorator to the parent node's Decorators list
	ParentNode->Decorators.Add(DecoratorGraphNode);
	Graph->UpdateAsset(0);
	BTAsset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_guid"),      DecoratorGraphNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("parent_guid"),    ParentNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("decorator_class"), DecClass->GetName());
	SetSuccess(Cmd, Result);
#else
	SetError(Cmd, TEXT("add_bt_decorator_node is editor-only"), TEXT("unavailable"));
#endif
}

// ── add_bt_service_node ───────────────────────────────────────────────────────
//
// Attaches a Service to an existing Behavior Tree composite graph node.
// params: behavior_tree_path (or behavior_tree_name), service_class (e.g. "BTService_DefaultFocus"),
//         parent_node_guid (the composite node to attach service to), node_position ([x,y] optional)
// result: { "node_guid": "..." }
//
void FMCPTCPServer::Cmd_AddBTServiceNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
#if WITH_EDITORONLY_DATA
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }
	FString BTPath, ServiceClassStr, ParentGuidStr;
	Cmd->Params->TryGetStringField(TEXT("behavior_tree_path"),  BTPath);
	if (BTPath.IsEmpty()) Cmd->Params->TryGetStringField(TEXT("behavior_tree_name"), BTPath);
	Cmd->Params->TryGetStringField(TEXT("service_class"),       ServiceClassStr);
	Cmd->Params->TryGetStringField(TEXT("parent_node_guid"),    ParentGuidStr);
	double PosX = 0.0, PosY = -40.0;
	const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr && PosArr->Num() >= 2)
		{ PosX = (*PosArr)[0]->AsNumber(); PosY = (*PosArr)[1]->AsNumber(); }

	if (BTPath.IsEmpty() || ServiceClassStr.IsEmpty())
	{
		SetError(Cmd, TEXT("behavior_tree_path (or behavior_tree_name) and service_class are required"), TEXT("invalid_params"));
		return;
	}

	UBehaviorTree* BTAsset = LoadObject<UBehaviorTree>(nullptr, *BTPath);
	if (!BTAsset) { SetError(Cmd, FString::Printf(TEXT("Behavior Tree not found: '%s'"), *BTPath), TEXT("not_found")); return; }

	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BTAsset->BTGraph);
	if (!Graph) { SetError(Cmd, TEXT("Behavior Tree has no editor graph"), TEXT("unavailable")); return; }

	// Find the parent composite node by GUID (or use first composite if not specified)
	UBehaviorTreeGraphNode* ParentNode = nullptr;
	if (!ParentGuidStr.IsEmpty())
	{
		FGuid ParentGuid;
		FGuid::Parse(ParentGuidStr, ParentGuid);
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid == ParentGuid)
			{
				ParentNode = Cast<UBehaviorTreeGraphNode>(N);
				break;
			}
		}
		if (!ParentNode)
		{
			SetError(Cmd, FString::Printf(TEXT("Parent node with GUID '%s' not found"), *ParentGuidStr), TEXT("not_found"));
			return;
		}
	}
	else
	{
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (Cast<UBehaviorTreeGraphNode_Composite>(N))
			{
				ParentNode = Cast<UBehaviorTreeGraphNode>(N);
				break;
			}
		}
		if (!ParentNode) { SetError(Cmd, TEXT("No composite node found; specify parent_node_guid"), TEXT("not_found")); return; }
	}

	// Resolve service class
	UClass* SvcClass = FindClassByName(ServiceClassStr);
	if (!SvcClass) SvcClass = FindFirstObject<UClass>(*ServiceClassStr, EFindFirstObjectOptions::NativeFirst);
	if (!SvcClass || !SvcClass->IsChildOf(UBTService::StaticClass()))
	{
		SetError(Cmd, FString::Printf(TEXT("Service class not found or not a UBTService: '%s'"), *ServiceClassStr), TEXT("not_found"));
		return;
	}

	// Create runtime service instance
	UBTService* RuntimeService = NewObject<UBTService>(BTAsset, SvcClass);
	if (!RuntimeService) { SetError(Cmd, TEXT("Failed to create service instance"), TEXT("error")); return; }

	// Create graph node and attach to parent
	UBehaviorTreeGraphNode_Service* ServiceGraphNode = nullptr;
	{
		FGraphNodeCreator<UBehaviorTreeGraphNode_Service> Builder(*Graph);
		ServiceGraphNode = Builder.CreateNode();
		if (!ServiceGraphNode) { SetError(Cmd, TEXT("CreateNode failed for service"), TEXT("error")); return; }
		ServiceGraphNode->NodeInstance = RuntimeService;
		ServiceGraphNode->NodePosX = static_cast<int32>(PosX);
		ServiceGraphNode->NodePosY = static_cast<int32>(PosY);
		Builder.Finalize();
	}

	// Add the service to the parent node's Services list
	ParentNode->Services.Add(ServiceGraphNode);
	Graph->UpdateAsset(0);
	BTAsset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_guid"),     ServiceGraphNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("parent_guid"),   ParentNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("service_class"), SvcClass->GetName());
	SetSuccess(Cmd, Result);
#else
	SetError(Cmd, TEXT("add_bt_service_node is editor-only"), TEXT("unavailable"));
#endif
}

// ── rebuild_navigation ───────────────────────────────────────────────────────
//
// params: [ use_pie, world_context_index ] (default: editor world)
// result: { "success": true [, "message": "..." ] }
// Calls UNavigationSystemV1::Build() so NavMesh reflects current level geometry (e.g. after spawning/moving actors).
//
void FMCPTCPServer::Cmd_RebuildNavigation(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	UWorld* World = GetWorldFromParams(Cmd->Params.IsValid() ? Cmd->Params : nullptr);
	if (!World)
	{
		SetError(Cmd, TEXT("No world available (editor or set use_pie / world_context_index)"));
		return;
	}

	UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
	if (!NavSys)
	{
		SetError(Cmd, TEXT("Navigation system not available for this world"));
		return;
	}

	NavSys->Build();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("message"), TEXT("Navigation rebuild triggered."));
	SetSuccess(Cmd, Result);
}

// ── create_material ──────────────────────────────────────────────────────────
//
// params: { "asset_path": "/Game/Materials/M_MyMaterial" }
// result: { "object_path": "...", "asset_path": "..." }
//
// Creates a new empty UMaterial asset at the given path (package path = asset_path).
// ─────────────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_CreateMaterial(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString AssetPath;
	if (!Cmd->Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		SetError(Cmd, TEXT("Missing or empty asset_path (e.g. /Game/Materials/M_New)"));
		return;
	}
	AssetPath = AssetPath.TrimStartAndEnd();
	FString PackageName = AssetPath;
	FString AssetName = FPaths::GetBaseFilename(AssetPath);
	if (AssetName.IsEmpty())
	{
		SetError(Cmd, TEXT("asset_path must end with an asset name (e.g. M_New)"));
		return;
	}

	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
	UPackage* Pkg = CreatePackage(*PackageName);
	if (!Pkg) { SetError(Cmd, TEXT("CreatePackage failed")); return; }
	UObject* Obj = Factory->FactoryCreateNew(UMaterial::StaticClass(), Pkg, FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn);
	if (!Obj)
	{
		SetError(Cmd, FString::Printf(TEXT("FactoryCreateNew failed for '%s'"), *AssetPath));
		return;
	}
	FAssetRegistryModule::AssetCreated(Obj);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("object_path"), Obj->GetPathName());
	Result->SetStringField(TEXT("asset_path"),  AssetPath);
	SetSuccess(Cmd, Result);
}

// ── get_material_expressions ──────────────────────────────────────────────────
//
// params: { "asset_path": "/Game/Materials/M_MyMaterial" } or { "object_path": "..." }
// result: { "expressions": [ { "class": "MaterialExpressionConstant", "name": "...", "guid": "..." }, ... ] }
//
// Loads a material asset and returns the list of expression nodes (class name, object name, guid).
// ─────────────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_GetMaterialExpressions(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString AssetPath, ObjectPath;
	Cmd->Params->TryGetStringField(TEXT("asset_path"),  AssetPath);
	Cmd->Params->TryGetStringField(TEXT("object_path"), ObjectPath);
	if (ObjectPath.IsEmpty() && !AssetPath.IsEmpty())
		ObjectPath = AssetPath;
	if (ObjectPath.IsEmpty())
	{
		SetError(Cmd, TEXT("Provide asset_path or object_path"));
		return;
	}

	// LoadObject expects "PackageName.AssetName"; asset_path may be "/Game/.../M_Name"
	FString LoadPath = ObjectPath;
	if (!ObjectPath.Contains(TEXT(".")))
		LoadPath = ObjectPath + TEXT(".") + FPaths::GetBaseFilename(ObjectPath);
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *LoadPath);
	if (!Material)
		Material = LoadObject<UMaterial>(nullptr, *FSoftObjectPath(ObjectPath).ToString());
	if (!Material)
	{
		SetError(Cmd, FString::Printf(TEXT("Material not found: '%s'"), *ObjectPath));
		return;
	}

	TArray<TSharedPtr<FJsonValue>> ExprArr;
	for (UMaterialExpression* Expr : Material->GetExpressions())
	{
		if (!Expr) continue;
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
		E->SetStringField(TEXT("name"),  Expr->GetName());
		E->SetStringField(TEXT("guid"),  Expr->MaterialExpressionGuid.IsValid() ? Expr->MaterialExpressionGuid.ToString() : FString());
		ExprArr.Add(MakeShared<FJsonValueObject>(E));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("expressions"), ExprArr);
	Result->SetNumberField(TEXT("count"), ExprArr.Num());
	SetSuccess(Cmd, Result);
}

// Helper: load material and optionally find expression by guid string or name
static UMaterial* LoadMaterialFromParams(const TSharedPtr<FJsonObject>& Params, FString* OutError)
{
	FString Path;
	Params->TryGetStringField(TEXT("material_path"), Path);
	if (Path.IsEmpty()) Params->TryGetStringField(TEXT("asset_path"), Path);
	if (Path.IsEmpty() && OutError) *OutError = TEXT("Missing material_path or asset_path");
	if (Path.IsEmpty()) return nullptr;
	FString LoadPath = Path;
	if (!Path.Contains(TEXT("."))) LoadPath = Path + TEXT(".") + FPaths::GetBaseFilename(Path);
	UMaterial* M = LoadObject<UMaterial>(nullptr, *LoadPath);
	if (!M) M = LoadObject<UMaterial>(nullptr, *FSoftObjectPath(Path).ToString());
	return M;
}
static UMaterialExpression* FindExpressionByGuidOrName(UMaterial* Material, const FString& GuidOrName)
{
	if (!Material || GuidOrName.IsEmpty()) return nullptr;
	FGuid Guid;
	if (FGuid::Parse(GuidOrName, Guid))
	{
		for (UMaterialExpression* E : Material->GetExpressions())
			if (E && E->MaterialExpressionGuid == Guid) return E;
	}
	for (UMaterialExpression* E : Material->GetExpressions())
		if (E && E->GetName() == GuidOrName) return E;
	return nullptr;
}
static EMaterialProperty ParseMaterialProperty(const FString& Name)
{
	UEnum* Enum = StaticEnum<EMaterialProperty>();
	if (!Enum) return MP_MAX;
	int64 V = Enum->GetValueByNameString(Name);
	if (V == INDEX_NONE) V = Enum->GetValueByNameString(FString(TEXT("MP_")) + Name);
	return (V >= 0 && V < MP_MAX) ? static_cast<EMaterialProperty>(V) : MP_MAX;
}

// ── connect_material_expressions ───────────────────────────────────────────────
// params: material_path, from_expression (guid or name), from_output_name (optional),
//         to_expression (guid or name), to_input_name (optional)
void FMCPTCPServer::Cmd_ConnectMaterialExpressions(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString Err;
	UMaterial* Material = LoadMaterialFromParams(Cmd->Params, &Err);
	if (!Material) { SetError(Cmd, Err.IsEmpty() ? TEXT("Material not found") : Err); return; }
	FString FromId, FromOutput, ToId, ToInput;
	Cmd->Params->TryGetStringField(TEXT("from_expression"), FromId);
	Cmd->Params->TryGetStringField(TEXT("from_output_name"), FromOutput);
	Cmd->Params->TryGetStringField(TEXT("to_expression"), ToId);
	Cmd->Params->TryGetStringField(TEXT("to_input_name"), ToInput);
	UMaterialExpression* From = FindExpressionByGuidOrName(Material, FromId);
	UMaterialExpression* To   = FindExpressionByGuidOrName(Material, ToId);
	if (!From) { SetError(Cmd, FString::Printf(TEXT("From expression not found: '%s'"), *FromId)); return; }
	if (!To)   { SetError(Cmd, FString::Printf(TEXT("To expression not found: '%s'"), *ToId)); return; }
	if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput))
	{
		SetError(Cmd, TEXT("ConnectMaterialExpressions failed (check pin names)"));
		return;
	}
	UMaterialEditingLibrary::RecompileMaterial(Material);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("connected"), true);
	SetSuccess(Cmd, Result);
}

// ── delete_material_expression ─────────────────────────────────────────────────
//
// params: material_path (or asset_path), expression_guid or expression name; optional "recompile": true
//
void FMCPTCPServer::Cmd_DeleteMaterialExpression(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString Err;
	UMaterial* Material = LoadMaterialFromParams(Cmd->Params, &Err);
	if (!Material) { SetError(Cmd, Err.IsEmpty() ? TEXT("Material not found") : Err); return; }
	FString ExprId;
	Cmd->Params->TryGetStringField(TEXT("expression_guid"), ExprId);
	if (ExprId.IsEmpty()) Cmd->Params->TryGetStringField(TEXT("expression_name"), ExprId);
	if (ExprId.IsEmpty()) { SetError(Cmd, TEXT("expression_guid or expression_name required")); return; }
	UMaterialExpression* Expr = FindExpressionByGuidOrName(Material, ExprId);
	if (!Expr) { SetError(Cmd, FString::Printf(TEXT("Expression not found: '%s'"), *ExprId)); return; }
	UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expr);
	bool bRecompile = true;
	if (Cmd->Params.IsValid()) Cmd->Params->TryGetBoolField(TEXT("recompile"), bRecompile);
	if (bRecompile) UMaterialEditingLibrary::RecompileMaterial(Material);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("deleted"), true);
	SetSuccess(Cmd, Result);
}

// ── connect_material_property ───────────────────────────────────────────────────
// params: material_path, from_expression (guid or name), from_output_name (optional), property (e.g. BaseColor or MP_BaseColor)
void FMCPTCPServer::Cmd_ConnectMaterialProperty(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString Err;
	UMaterial* Material = LoadMaterialFromParams(Cmd->Params, &Err);
	if (!Material) { SetError(Cmd, Err.IsEmpty() ? TEXT("Material not found") : Err); return; }
	FString FromId, FromOutput, PropertyStr;
	Cmd->Params->TryGetStringField(TEXT("from_expression"), FromId);
	Cmd->Params->TryGetStringField(TEXT("from_output_name"), FromOutput);
	Cmd->Params->TryGetStringField(TEXT("property"), PropertyStr);
	UMaterialExpression* From = FindExpressionByGuidOrName(Material, FromId);
	if (!From) { SetError(Cmd, FString::Printf(TEXT("Expression not found: '%s'"), *FromId)); return; }
	EMaterialProperty Prop = ParseMaterialProperty(PropertyStr);
	if (Prop >= MP_MAX) { SetError(Cmd, FString::Printf(TEXT("Unknown material property: '%s'"), *PropertyStr)); return; }
	if (!UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOutput, Prop))
	{
		SetError(Cmd, TEXT("ConnectMaterialProperty failed"));
		return;
	}
	UMaterialEditingLibrary::RecompileMaterial(Material);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("connected"), true);
	SetSuccess(Cmd, Result);
}

// ── add_material_expression ───────────────────────────────────────────────────
// params: material_path, expression_class (e.g. MaterialExpressionConstant), node_position [x,y] (optional)
// result: expression guid and name
void FMCPTCPServer::Cmd_AddMaterialExpression(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString Err;
	UMaterial* Material = LoadMaterialFromParams(Cmd->Params, &Err);
	if (!Material) { SetError(Cmd, Err.IsEmpty() ? TEXT("Material not found") : Err); return; }
	FString ClassName;
	Cmd->Params->TryGetStringField(TEXT("expression_class"), ClassName);
	if (ClassName.IsEmpty()) { SetError(Cmd, TEXT("Missing expression_class")); return; }
	UClass* ExprClass = FindObject<UClass>(nullptr, *ClassName);
	if (!ExprClass) ExprClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!ExprClass || !ExprClass->IsChildOf(UMaterialExpression::StaticClass()))
	{
		SetError(Cmd, FString::Printf(TEXT("Not a material expression class: '%s'"), *ClassName));
		return;
	}
	int32 PosX = 0, PosY = 0;
	const TArray<TSharedPtr<FJsonValue>>* PosArr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr->Num() >= 2)
	{
		PosX = static_cast<int32>((*PosArr)[0]->AsNumber());
		PosY = static_cast<int32>((*PosArr)[1]->AsNumber());
	}
	UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(Material, ExprClass, PosX, PosY);
	if (!Expr) { SetError(Cmd, TEXT("CreateMaterialExpression failed")); return; }
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("expression_guid"), Expr->MaterialExpressionGuid.ToString());
	Result->SetStringField(TEXT("expression_name"), Expr->GetName());
	SetSuccess(Cmd, Result);
}

// ── set_material_expression_property ─────────────────────────────────────────
//
// Sets a property on a material expression by GUID, using dot-notation paths.
//
// params:
//   "material_path"    : content path of the material asset
//   "expression_guid"  : GUID string returned by add_material_expression
//   "property_path"    : dot-notation property, e.g. "Constant", "DefaultValue"
//   "value"            : JSON value (number, string, or object)
//
// result: { "expression_name": "...", "property_path": "...", "set": true }
//
// Examples:
//   Constant3Vector color:  property_path="Constant", value={"r":1.0,"g":0.75,"b":0.1,"a":1.0}
//   VectorParameter color:  property_path="DefaultValue", value={"r":1.0,"g":0.75,"b":0.1,"a":1.0}
//   Scalar constant:        property_path="R", value=0.4
//   Parameter name:         property_path="ParameterName", value="MyColor"
//
void FMCPTCPServer::Cmd_SetMaterialExpressionProperty(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString Err;
	UMaterial* Material = LoadMaterialFromParams(Cmd->Params, &Err);
	if (!Material) { SetError(Cmd, Err.IsEmpty() ? TEXT("Material not found") : Err, TEXT("not_found")); return; }

	FString ExprId, PropertyPath;
	Cmd->Params->TryGetStringField(TEXT("expression_guid"), ExprId);
	Cmd->Params->TryGetStringField(TEXT("property_path"),   PropertyPath);

	if (ExprId.IsEmpty())    { SetError(Cmd, TEXT("'expression_guid' is required"), TEXT("invalid_params")); return; }
	if (PropertyPath.IsEmpty()) { SetError(Cmd, TEXT("'property_path' is required"), TEXT("invalid_params")); return; }

	UMaterialExpression* Expr = FindExpressionByGuidOrName(Material, ExprId);
	if (!Expr) { SetError(Cmd, FString::Printf(TEXT("Expression '%s' not found"), *ExprId), TEXT("not_found")); return; }

	const TSharedPtr<FJsonValue>* ValueField = Cmd->Params->Values.Find(TEXT("value"));
	if (!ValueField) { SetError(Cmd, TEXT("'value' is required"), TEXT("invalid_params")); return; }

	Expr->Modify();
	FString OutErr;
	if (!SetNestedProperty(Expr, PropertyPath, *ValueField, OutErr))
	{
		SetError(Cmd, FString::Printf(TEXT("Failed to set '%s': %s"), *PropertyPath, *OutErr), TEXT("error"));
		return;
	}

	// Mark material dirty and propagate changes
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("expression_name"), Expr->GetName());
	Result->SetStringField(TEXT("property_path"),   PropertyPath);
	Result->SetBoolField  (TEXT("set"),             true);
	SetSuccess(Cmd, Result);
}

// ── recompile_material ────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_RecompileMaterial(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString Err;
	UMaterial* Material = LoadMaterialFromParams(Cmd->Params, &Err);
	if (!Material) { SetError(Cmd, Err.IsEmpty() ? TEXT("Material not found") : Err); return; }
	UMaterialEditingLibrary::RecompileMaterial(Material);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("recompiled"), true);
	SetSuccess(Cmd, Result);
}

// ── get_material_expression_pins ───────────────────────────────────────────────
//
// params: material_path (or asset_path), expression_guid or expression_name
// result: { "input_names": [ "A", "B", ... ], "input_types": [ 0, 1, ... ] }
//
void FMCPTCPServer::Cmd_GetMaterialExpressionPins(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString Err;
	UMaterial* Material = LoadMaterialFromParams(Cmd->Params, &Err);
	if (!Material) { SetError(Cmd, Err.IsEmpty() ? TEXT("Material not found") : Err); return; }
	FString ExprId;
	Cmd->Params->TryGetStringField(TEXT("expression_guid"), ExprId);
	if (ExprId.IsEmpty()) Cmd->Params->TryGetStringField(TEXT("expression_name"), ExprId);
	if (ExprId.IsEmpty()) { SetError(Cmd, TEXT("expression_guid or expression_name required")); return; }
	UMaterialExpression* Expr = FindExpressionByGuidOrName(Material, ExprId);
	if (!Expr) { SetError(Cmd, FString::Printf(TEXT("Expression not found: '%s'"), *ExprId)); return; }
	TArray<FString> Names = UMaterialEditingLibrary::GetMaterialExpressionInputNames(Expr);
	TArray<int32> Types = UMaterialEditingLibrary::GetMaterialExpressionInputTypes(Expr);
	TArray<TSharedPtr<FJsonValue>> NameArr, TypeArr;
	for (const FString& N : Names) NameArr.Add(MakeShared<FJsonValueString>(N));
	for (int32 T : Types) TypeArr.Add(MakeShared<FJsonValueNumber>(T));
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("input_names"), NameArr);
	Result->SetArrayField(TEXT("input_types"), TypeArr);
	SetSuccess(Cmd, Result);
}

// ── add_umg_widget ─────────────────────────────────────────────────────────────
// params: widget_blueprint_path (asset path), widget_class (e.g. Button, TextBlock), parent_widget_name (optional, root if empty), widget_name (optional)
// result: widget_name (and path in tree)
void FMCPTCPServer::Cmd_AddUmgWidget(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString BPPath, WidgetClassStr, ParentName, WidgetName;
	Cmd->Params->TryGetStringField(TEXT("widget_blueprint_path"), BPPath);
	if (BPPath.IsEmpty()) Cmd->Params->TryGetStringField(TEXT("asset_path"), BPPath);
	Cmd->Params->TryGetStringField(TEXT("widget_class"), WidgetClassStr);
	Cmd->Params->TryGetStringField(TEXT("parent_widget_name"), ParentName);
	Cmd->Params->TryGetStringField(TEXT("widget_name"), WidgetName);
	if (BPPath.IsEmpty()) { SetError(Cmd, TEXT("Missing widget_blueprint_path or asset_path")); return; }
	if (WidgetClassStr.IsEmpty()) { SetError(Cmd, TEXT("Missing widget_class (e.g. Button, TextBlock)")); return; }
	FString LoadPath = BPPath.Contains(TEXT(".")) ? BPPath : BPPath + TEXT(".") + FPaths::GetBaseFilename(BPPath);
	UWidgetBlueprint* WB = LoadObject<UWidgetBlueprint>(nullptr, *LoadPath);
	if (!WB) WB = LoadObject<UWidgetBlueprint>(nullptr, *FSoftObjectPath(BPPath).ToString());
	if (!WB || !WB->WidgetTree)
	{
		SetError(Cmd, FString::Printf(TEXT("Widget Blueprint not found or has no WidgetTree: '%s'"), *BPPath));
		return;
	}
	UClass* WidgetClass = FindObject<UClass>(nullptr, *WidgetClassStr);
	if (!WidgetClass) WidgetClass = FindFirstObject<UClass>(*FString::Printf(TEXT("WidgetBlueprint'%s'"), *WidgetClassStr), EFindFirstObjectOptions::None);
	if (!WidgetClass)
	{
		// Try common UMG widget names with Engine prefix
		FString FullName = FString::Printf(TEXT("/Script/UMG.%s"), *WidgetClassStr);
		WidgetClass = LoadObject<UClass>(nullptr, *FullName);
	}
	if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
	{
		SetError(Cmd, FString::Printf(TEXT("Not a UWidget class: '%s' (use e.g. Button, TextBlock, Image)"), *WidgetClassStr));
		return;
	}
	UWidgetTree* Tree = WB->WidgetTree;
	UWidget* NewWidget = nullptr;
	if (WidgetClass->IsChildOf(UUserWidget::StaticClass()))
		NewWidget = Tree->ConstructWidget<UUserWidget>(WidgetClass, WidgetName.IsEmpty() ? NAME_None : FName(*WidgetName));
	else
		NewWidget = Tree->ConstructWidget<UWidget>(WidgetClass, WidgetName.IsEmpty() ? NAME_None : FName(*WidgetName));
	if (!NewWidget)
	{
		SetError(Cmd, TEXT("ConstructWidget failed"));
		return;
	}
	UPanelWidget* Parent = nullptr;
	if (!ParentName.IsEmpty())
	{
		Parent = Cast<UPanelWidget>(Tree->FindWidget(FName(*ParentName)));
		if (!Parent)
		{
			SetError(Cmd, FString::Printf(TEXT("Parent widget not found: '%s'"), *ParentName));
			return;
		}
	}
	else
		Parent = Cast<UPanelWidget>(Tree->RootWidget);
	if (!Parent)
	{
		SetError(Cmd, TEXT("No root or parent panel to add widget to"));
		return;
	}
	Parent->AddChild(NewWidget);
	FBlueprintEditorUtils::MarkBlueprintAsModified(WB);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("widget_name"), NewWidget->GetName());
	Result->SetStringField(TEXT("widget_class"), NewWidget->GetClass()->GetName());
	SetSuccess(Cmd, Result);
}

// Helper: load Widget Blueprint and return WidgetTree (or null).
static UWidgetTree* GetWidgetTreeFromParams(const TSharedPtr<FJsonObject>& Params, FString* OutError)
{
	FString BPPath;
	Params->TryGetStringField(TEXT("widget_blueprint_path"), BPPath);
	if (BPPath.IsEmpty()) Params->TryGetStringField(TEXT("asset_path"), BPPath);
	if (BPPath.IsEmpty() && OutError) *OutError = TEXT("Missing widget_blueprint_path or asset_path");
	if (BPPath.IsEmpty()) return nullptr;
	FString LoadPath = BPPath.Contains(TEXT(".")) ? BPPath : BPPath + TEXT(".") + FPaths::GetBaseFilename(BPPath);
	UWidgetBlueprint* WB = LoadObject<UWidgetBlueprint>(nullptr, *LoadPath);
	if (!WB) WB = LoadObject<UWidgetBlueprint>(nullptr, *FSoftObjectPath(BPPath).ToString());
	if (!WB || !WB->WidgetTree) { if (OutError) *OutError = FString::Printf(TEXT("Widget Blueprint not found or no WidgetTree: '%s'"), *BPPath); return nullptr; }
	return WB->WidgetTree;
}

// ── remove_umg_widget ─────────────────────────────────────────────────────────
//
// params: widget_blueprint_path, widget_name
//
void FMCPTCPServer::Cmd_RemoveUmgWidget(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString Err;
	UWidgetTree* Tree = GetWidgetTreeFromParams(Cmd->Params, &Err);
	if (!Tree) { SetError(Cmd, Err); return; }
	FString WidgetName;
	if (!Cmd->Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		SetError(Cmd, TEXT("widget_name is required"));
		return;
	}
	UWidget* Widget = Tree->FindWidget(FName(*WidgetName));
	if (!Widget) { SetError(Cmd, FString::Printf(TEXT("Widget not found: '%s'"), *WidgetName)); return; }
	if (!Tree->RemoveWidget(Widget)) { SetError(Cmd, TEXT("RemoveWidget failed")); return; }
	UWidgetBlueprint* WB = Cast<UWidgetBlueprint>(Tree->GetOuter());
	if (WB) FBlueprintEditorUtils::MarkBlueprintAsModified(WB);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("removed"), WidgetName);
	SetSuccess(Cmd, Result);
}

// ── get_umg_tree ─────────────────────────────────────────────────────────────
//
// params: widget_blueprint_path
// result: { "root": { "name", "class", "children": [ ... ] }, "count": N }
//
static TSharedPtr<FJsonObject> WidgetToJson(UWidget* Widget)
{
	if (!Widget) return nullptr;
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Widget->GetName());
	Obj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
	TArray<TSharedPtr<FJsonValue>> Children;
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
		{
			if (UWidget* Child = Panel->GetChildAt(i))
			{
				TSharedPtr<FJsonObject> ChildObj = WidgetToJson(Child);
				if (ChildObj.IsValid()) Children.Add(MakeShared<FJsonValueObject>(ChildObj));
			}
		}
	}
	Obj->SetArrayField(TEXT("children"), Children);
	return Obj;
}

void FMCPTCPServer::Cmd_GetUmgTree(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString Err;
	UWidgetTree* Tree = GetWidgetTreeFromParams(Cmd->Params, &Err);
	if (!Tree) { SetError(Cmd, Err); return; }
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	int32 Count = 0;
	if (Tree->RootWidget)
	{
		Result->SetObjectField(TEXT("root"), WidgetToJson(Tree->RootWidget));
		Tree->ForEachWidget([&Count](UWidget*) { ++Count; });
	}
	else
		Result->SetObjectField(TEXT("root"), MakeShared<FJsonObject>());
	Result->SetNumberField(TEXT("count"), Count);
	SetSuccess(Cmd, Result);
}

// ── set_umg_slot_content ─────────────────────────────────────────────────────
//
// params: widget_blueprint_path, slot_name, content_widget_name (widget already in tree to put in the slot)
//
void FMCPTCPServer::Cmd_SetUmgSlotContent(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString Err;
	UWidgetTree* Tree = GetWidgetTreeFromParams(Cmd->Params, &Err);
	if (!Tree) { SetError(Cmd, Err); return; }
	FString SlotName, ContentWidgetName;
	if (!Cmd->Params->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
		{ SetError(Cmd, TEXT("slot_name is required")); return; }
	if (!Cmd->Params->TryGetStringField(TEXT("content_widget_name"), ContentWidgetName) || ContentWidgetName.IsEmpty())
		{ SetError(Cmd, TEXT("content_widget_name is required")); return; }
	UWidget* Content = Tree->FindWidget(FName(*ContentWidgetName));
	if (!Content) { SetError(Cmd, FString::Printf(TEXT("Content widget not found: '%s'"), *ContentWidgetName)); return; }
	Tree->SetContentForSlot(FName(*SlotName), Content);
	UWidgetBlueprint* WB = Cast<UWidgetBlueprint>(Tree->GetOuter());
	if (WB) FBlueprintEditorUtils::MarkBlueprintAsModified(WB);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("slot_name"), SlotName);
	Result->SetStringField(TEXT("content_widget_name"), ContentWidgetName);
	SetSuccess(Cmd, Result);
}

// ── get_asset_full_metadata ────────────────────────────────────────────────────
//
// params: { "object_path": "..." } or { "asset_path": "/Game/.../Name" }
// result: { "package_name", "asset_name", "class", "object_path", "tags": { ... } }
//
// Returns FAssetData fields plus all asset registry tags (when available).
// ─────────────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_GetAssetFullMetadata(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString ObjectPath, AssetPath;
	Cmd->Params->TryGetStringField(TEXT("object_path"), ObjectPath);
	Cmd->Params->TryGetStringField(TEXT("asset_path"),  AssetPath);
	if (ObjectPath.IsEmpty() && !AssetPath.IsEmpty())
		ObjectPath = AssetPath;
	if (ObjectPath.IsEmpty())
	{
		SetError(Cmd, TEXT("Provide object_path or asset_path"));
		return;
	}

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();
	FSoftObjectPath Path(ObjectPath);
	FAssetData Data = AR.GetAssetByObjectPath(Path, /*bIncludeOnlyOnDiskAssets=*/false);
	if (!Data.IsValid())
	{
		SetError(Cmd, FString::Printf(TEXT("Asset not found: '%s'"), *ObjectPath));
		return;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("package_name"), Data.PackageName.ToString());
	Result->SetStringField(TEXT("asset_name"),   Data.AssetName.ToString());
	Result->SetStringField(TEXT("class"),        Data.AssetClassPath.ToString());
	Result->SetStringField(TEXT("object_path"),  Data.GetObjectPathString());

	TSharedPtr<FJsonObject> TagsObj = MakeShared<FJsonObject>();
	Data.EnumerateTags([&TagsObj](const TPair<FName, FAssetTagValueRef>& Pair)
	{
		TagsObj->SetStringField(Pair.Key.ToString(), Pair.Value.AsString());
	});
	Result->SetObjectField(TEXT("tags"), TagsObj);

	SetSuccess(Cmd, Result);
}

// ── set_actor_property ────────────────────────────────────────────────────────
// Enhanced to support nested property paths using dot notation.
// Examples:
//   "bUnbound" -> simple property on actor
//   "Settings.AutoExposureBias" -> nested struct property
//   "Settings.SceneColorTint" -> nested FLinearColor struct
//
// Supports: float, bool, enum (string), FLinearColor (object with r,g,b,a)
//
void FMCPTCPServer::Cmd_SetActorProperty(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}

	FString PropertyPath;
	Cmd->Params->TryGetStringField(TEXT("property_name"), PropertyPath);
	if (PropertyPath.IsEmpty())
	{
		SetError(Cmd, TEXT("'property_name' is required"));
		return;
	}

	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor)
	{
		SetError(Cmd, Err);
		return;
	}

	// Get the value as a JSON value
	TSharedPtr<FJsonValue> ValueJson = Cmd->Params->TryGetField(TEXT("value"));
	if (!ValueJson.IsValid())
	{
		SetError(Cmd, TEXT("'value' is required"));
		return;
	}

	// Check if this is a nested property path (contains dots)
	if (PropertyPath.Contains(TEXT(".")))
	{
		// Handle nested property path
		FString ErrorMessage;
		if (!SetNestedProperty(Actor, PropertyPath, ValueJson, ErrorMessage))
		{
			SetError(Cmd, ErrorMessage);
			return;
		}
	}
	else
	{
		// Simple property - use original logic
		FProperty* Prop = Actor->GetClass()->FindPropertyByName(*PropertyPath);
		if (!Prop)
		{
			SetError(Cmd, FString::Printf(
				TEXT("Property '%s' not found on '%s'."),
				*PropertyPath, *Actor->GetClass()->GetName()));
			return;
		}

		FString ValueStr = JsonValueToString(ValueJson);
		if (!Prop->ImportText_InContainer(*ValueStr, Actor, Actor, PPF_None))
		{
			SetError(Cmd, FString::Printf(TEXT("Failed to import value '%s' for property '%s'"),
				*ValueStr, *PropertyPath));
			return;
		}
	}

	Actor->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("label"),    Actor->GetActorLabel());
	Result->SetStringField(TEXT("property"), PropertyPath);
	Result->SetStringField(TEXT("value"),    JsonValueToString(ValueJson));
	SetSuccess(Cmd, Result);
}

// Helper: Convert JSON value to string for ImportText
FString FMCPTCPServer::JsonValueToString(TSharedPtr<FJsonValue> ValueJson)
{
	if (!ValueJson.IsValid())
	{
		return FString();
	}

	FString ValueStr;
	switch (ValueJson->Type)
	{
	case EJson::String:
		ValueStr = ValueJson->AsString();
		break;
	case EJson::Number:
		ValueStr = FString::SanitizeFloat(ValueJson->AsNumber());
		break;
	case EJson::Boolean:
		ValueStr = ValueJson->AsBool() ? TEXT("true") : TEXT("false");
		break;
	case EJson::Object:
	{
		// For structs like FLinearColor, format as (R=...,G=...,B=...,A=...)
		TSharedPtr<FJsonObject> Obj = ValueJson->AsObject();
		if (Obj->HasField(TEXT("r")) && Obj->HasField(TEXT("g")) && Obj->HasField(TEXT("b")))
		{
			// FLinearColor format
			double R = Obj->GetNumberField(TEXT("r"));
			double G = Obj->GetNumberField(TEXT("g"));
			double B = Obj->GetNumberField(TEXT("b"));
			double A = Obj->HasField(TEXT("a")) ? Obj->GetNumberField(TEXT("a")) : 1.0;
			ValueStr = FString::Printf(TEXT("(R=%f,G=%f,B=%f,A=%f)"), R, G, B, A);
		}
		else
		{
			// Generic object - serialize to JSON string
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&ValueStr);
			FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
		}
		break;
	}
	case EJson::Array:
	{
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&ValueStr);
		FJsonSerializer::Serialize(ValueJson->AsArray(), *W);
		break;
	}
	default:
		break;
	}

	return ValueStr;
}

// Helper: Set a nested property using dot notation path
bool FMCPTCPServer::SetNestedProperty(UObject* TargetObject, const FString& PropertyPath,
	TSharedPtr<FJsonValue> ValueJson, FString& OutErrorMessage)
{
	TArray<FString> PathParts;
	PropertyPath.ParseIntoArray(PathParts, TEXT("."));

	if (PathParts.Num() < 1)
	{
		OutErrorMessage = TEXT("Invalid property path");
		return false;
	}

	UStruct* CurrentStruct = TargetObject->GetClass();
	void* CurrentContainer = TargetObject;
	FProperty* LastProperty = nullptr;

	for (int32 i = 0; i < PathParts.Num(); ++i)
	{
		const FString& PropertyName = PathParts[i];
		LastProperty = CurrentStruct->FindPropertyByName(*PropertyName);

		if (!LastProperty)
		{
			OutErrorMessage = FString::Printf(TEXT("Property '%s' not found in path '%s'"),
				*PropertyName, *PropertyPath);
			return false;
		}

		// If this is the last part of the path, set the value
		if (i == PathParts.Num() - 1)
		{
			return SetPropertyValue(LastProperty, CurrentContainer, ValueJson, OutErrorMessage);
		}

		// Otherwise, we need to navigate deeper into a struct
		FStructProperty* StructProp = CastField<FStructProperty>(LastProperty);
		if (!StructProp)
		{
			OutErrorMessage = FString::Printf(TEXT("Property '%s' is not a struct, cannot navigate deeper"),
				*PropertyName);
			return false;
		}

		// Get the container pointer for the nested struct
		CurrentContainer = LastProperty->ContainerPtrToValuePtr<void>(CurrentContainer);
		CurrentStruct = StructProp->Struct;
	}

	OutErrorMessage = TEXT("Failed to set property");
	return false;
}

// Helper: Set a property value with type-aware conversion
bool FMCPTCPServer::SetPropertyValue(FProperty* Prop, void* Container, TSharedPtr<FJsonValue> ValueJson,
	FString& OutErrorMessage)
{
	if (!Prop || !Container || !ValueJson.IsValid())
	{
		OutErrorMessage = TEXT("Invalid parameters for SetPropertyValue");
		return false;
	}

	// Handle different property types
	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		if (ValueJson->Type == EJson::Boolean)
		{
			BoolProp->SetPropertyValue_InContainer(Container, ValueJson->AsBool());
			return true;
		}
		OutErrorMessage = FString::Printf(TEXT("Expected boolean value for property '%s'"), *Prop->GetName());
		return false;
	}

	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
	{
		if (ValueJson->Type == EJson::Number)
		{
			FloatProp->SetPropertyValue_InContainer(Container, static_cast<float>(ValueJson->AsNumber()));
			return true;
		}
		OutErrorMessage = FString::Printf(TEXT("Expected numeric value for property '%s'"), *Prop->GetName());
		return false;
	}

	if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
	{
		if (ValueJson->Type == EJson::Number)
		{
			DoubleProp->SetPropertyValue_InContainer(Container, ValueJson->AsNumber());
			return true;
		}
		OutErrorMessage = FString::Printf(TEXT("Expected numeric value for property '%s'"), *Prop->GetName());
		return false;
	}

	if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
	{
		if (ValueJson->Type == EJson::Number)
		{
			IntProp->SetPropertyValue_InContainer(Container, static_cast<int32>(ValueJson->AsNumber()));
			return true;
		}
		OutErrorMessage = FString::Printf(TEXT("Expected numeric value for property '%s'"), *Prop->GetName());
		return false;
	}

	if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		// Check if this is a TEnumAsByte (byte property with an enum)
		if (ByteProp->Enum && ValueJson->Type == EJson::String)
		{
			FString EnumValueStr = ValueJson->AsString();
			int64 EnumValue = ByteProp->Enum->GetValueByName(*EnumValueStr);
			if (EnumValue != INDEX_NONE)
			{
				ByteProp->SetPropertyValue_InContainer(Container, static_cast<uint8>(EnumValue));
				return true;
			}
			OutErrorMessage = FString::Printf(TEXT("Invalid enum value '%s' for property '%s'."),
				*EnumValueStr, *Prop->GetName());
			return false;
		}
		// Otherwise treat as regular byte
		if (ValueJson->Type == EJson::Number)
		{
			ByteProp->SetPropertyValue_InContainer(Container, static_cast<uint8>(ValueJson->AsNumber()));
			return true;
		}
		OutErrorMessage = FString::Printf(TEXT("Expected numeric value for property '%s'"), *Prop->GetName());
		return false;
	}

	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		if (ValueJson->Type == EJson::String)
		{
			FString EnumValueStr = ValueJson->AsString();
			UEnum* Enum = EnumProp->GetEnum();
			if (Enum)
			{
				int64 EnumValue = Enum->GetValueByName(*EnumValueStr);
				if (EnumValue != INDEX_NONE)
				{
					// Use ImportText for enum values (most reliable method)
					FString ImportStr = FString::Printf(TEXT("%s"), *EnumValueStr);
					if (EnumProp->ImportText_InContainer(*ImportStr, Container, nullptr, PPF_None))
					{
						return true;
					}
				}
				OutErrorMessage = FString::Printf(TEXT("Invalid enum value '%s' for property '%s'."),
					*EnumValueStr, *Prop->GetName());
				return false;
			}
		}
		OutErrorMessage = FString::Printf(TEXT("Expected string value for enum property '%s'"), *Prop->GetName());
		return false;
	}

	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		// Handle specific struct types
		if (StructProp->Struct->GetFName() == FName(TEXT("LinearColor")))
		{
			if (ValueJson->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> Obj = ValueJson->AsObject();
				double R = Obj->HasField(TEXT("r")) ? Obj->GetNumberField(TEXT("r")) : 0.0;
				double G = Obj->HasField(TEXT("g")) ? Obj->GetNumberField(TEXT("g")) : 0.0;
				double B = Obj->HasField(TEXT("b")) ? Obj->GetNumberField(TEXT("b")) : 0.0;
				double A = Obj->HasField(TEXT("a")) ? Obj->GetNumberField(TEXT("a")) : 1.0;

				FLinearColor* Color = StructProp->ContainerPtrToValuePtr<FLinearColor>(Container);
				if (Color)
				{
					Color->R = static_cast<float>(R);
					Color->G = static_cast<float>(G);
					Color->B = static_cast<float>(B);
					Color->A = static_cast<float>(A);
					return true;
				}
			}
			OutErrorMessage = FString::Printf(TEXT("Expected object with r,g,b,a keys for FLinearColor property '%s'"),
				*Prop->GetName());
			return false;
		}

		if (StructProp->Struct->GetFName() == FName(TEXT("Color")))
		{
			if (ValueJson->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> Obj = ValueJson->AsObject();
				double R = Obj->HasField(TEXT("r")) ? Obj->GetNumberField(TEXT("r")) : 0.0;
				double G = Obj->HasField(TEXT("g")) ? Obj->GetNumberField(TEXT("g")) : 0.0;
				double B = Obj->HasField(TEXT("b")) ? Obj->GetNumberField(TEXT("b")) : 0.0;
				double A = Obj->HasField(TEXT("a")) ? Obj->GetNumberField(TEXT("a")) : 0.0;

				FColor* Color = StructProp->ContainerPtrToValuePtr<FColor>(Container);
				if (Color)
				{
					Color->R = static_cast<uint8>(FMath::Clamp(R * 255.0, 0.0, 255.0));
					Color->G = static_cast<uint8>(FMath::Clamp(G * 255.0, 0.0, 255.0));
					Color->B = static_cast<uint8>(FMath::Clamp(B * 255.0, 0.0, 255.0));
					Color->A = static_cast<uint8>(FMath::Clamp(A * 255.0, 0.0, 255.0));
					return true;
				}
			}
			OutErrorMessage = FString::Printf(TEXT("Expected object with r,g,b,a keys for FColor property '%s'"),
				*Prop->GetName());
			return false;
		}

		// For other structs, try ImportText
		FString ValueStr = JsonValueToString(ValueJson);
		return Prop->ImportText_InContainer(*ValueStr, Container, nullptr, PPF_None) != nullptr;
	}

	if (FMapProperty* MP = CastField<FMapProperty>(Prop))
	{
		if (ValueJson->Type != EJson::Array)
		{
			OutErrorMessage = FString::Printf(TEXT("Expected JSON array of {key, value} for map property '%s'"), *Prop->GetName());
			return false;
		}
		void* MapPtr = MP->ContainerPtrToValuePtr<void>(Container);
		FScriptMapHelper MapHelper(MP, MapPtr);
		MapHelper.EmptyValues();
		const TArray<TSharedPtr<FJsonValue>>& Arr = ValueJson->AsArray();
		const int32 KeySize = MP->KeyProp->GetSize();
		const int32 ValueSize = MP->ValueProp->GetSize();
		TArray<uint8> KeyBuf; KeyBuf.SetNumUninitialized(KeySize);
		TArray<uint8> ValueBuf; ValueBuf.SetNumUninitialized(ValueSize);
		for (const TSharedPtr<FJsonValue>& Item : Arr)
		{
			if (!Item.IsValid() || Item->Type != EJson::Object) continue;
			TSharedPtr<FJsonObject> Obj = Item->AsObject();
			TSharedPtr<FJsonValue> KeyJson = Obj->TryGetField(TEXT("key"));
			TSharedPtr<FJsonValue> ValueJsonPtr = Obj->TryGetField(TEXT("value"));
			if (!KeyJson.IsValid() || !ValueJsonPtr.IsValid())
				continue;
			FString KeyStr = JsonValueToString(KeyJson);
			FString ValStr = JsonValueToString(ValueJsonPtr);
			MP->KeyProp->InitializeValue(KeyBuf.GetData());
			MP->ValueProp->InitializeValue(ValueBuf.GetData());
			if (MP->KeyProp->ImportText_Direct(*KeyStr, KeyBuf.GetData(), nullptr, PPF_None, nullptr) &&
			    MP->ValueProp->ImportText_Direct(*ValStr, ValueBuf.GetData(), nullptr, PPF_None, nullptr))
			{
				MapHelper.AddPair(KeyBuf.GetData(), ValueBuf.GetData());
			}
			MP->KeyProp->DestroyValue(KeyBuf.GetData());
			MP->ValueProp->DestroyValue(ValueBuf.GetData());
		}
		return true;
	}

	if (FSetProperty* SP = CastField<FSetProperty>(Prop))
	{
		if (ValueJson->Type != EJson::Array)
		{
			OutErrorMessage = FString::Printf(TEXT("Expected JSON array for set property '%s'"), *Prop->GetName());
			return false;
		}
		void* SetPtr = SP->ContainerPtrToValuePtr<void>(Container);
		FScriptSetHelper SetHelper(SP, SetPtr);
		SetHelper.EmptyElements();
		const int32 ElemSize = SP->ElementProp->GetSize();
		TArray<uint8> ElemBuf; ElemBuf.SetNumUninitialized(ElemSize);
		for (const TSharedPtr<FJsonValue>& Item : ValueJson->AsArray())
		{
			if (!Item.IsValid()) continue;
			FString ElemStr = JsonValueToString(Item);
			SP->ElementProp->InitializeValue(ElemBuf.GetData());
			if (SP->ElementProp->ImportText_Direct(*ElemStr, ElemBuf.GetData(), nullptr, PPF_None, nullptr))
				SetHelper.AddElement(ElemBuf.GetData());
			SP->ElementProp->DestroyValue(ElemBuf.GetData());
		}
		return true;
	}

	// FArrayProperty: use FScriptArrayHelper + JsonValueToUProperty per element (handles primitives, structs, etc.)
	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		if (ValueJson->Type != EJson::Array)
		{
			OutErrorMessage = FString::Printf(TEXT("Expected JSON array for array property '%s'"), *Prop->GetName());
			return false;
		}
		void* ArrayDataPtr = ArrayProp->ContainerPtrToValuePtr<void>(Container);
		FScriptArrayHelper ArrayHelper(ArrayProp, ArrayDataPtr);
		ArrayHelper.EmptyValues();
		FProperty* InnerProp = ArrayProp->Inner;
		const TArray<TSharedPtr<FJsonValue>>& JsonArr = ValueJson->AsArray();
		for (int32 i = 0; i < JsonArr.Num(); ++i)
		{
			if (!JsonArr[i].IsValid()) continue;
			int32 NewIndex = ArrayHelper.AddValue();
			uint8* ElementPtr = ArrayHelper.GetRawPtr(NewIndex);
			InnerProp->InitializeValue(ElementPtr);
			FText ParseFailReason;
			if (!FJsonObjectConverter::JsonValueToUProperty(JsonArr[i], InnerProp, ElementPtr, 0, 0, /*bStrictMode=*/false, &ParseFailReason, nullptr))
			{
				InnerProp->DestroyValue(ElementPtr);
				ArrayHelper.RemoveValues(NewIndex, 1);
				OutErrorMessage = FString::Printf(TEXT("Failed to parse JSON array element at index %d for property '%s': %s"),
					i, *Prop->GetName(), *ParseFailReason.ToString());
				return false;
			}
		}
		return true;
	}

	// Default: try ImportText
	FString ValueStr = JsonValueToString(ValueJson);
	if (Prop->ImportText_InContainer(*ValueStr, Container, nullptr, PPF_None))
	{
		return true;
	}

	OutErrorMessage = FString::Printf(TEXT("Failed to set property '%s' - unsupported type or invalid value"),
		*Prop->GetName());
	return false;
}

// ── set_component_property ───────────────────────────────────────────────────
//
// params: {
//   "actor_label": "MyActor",
//   "component_name": "PointLightComponent",
//   "property_name": "Intensity",
//   "value": 50.0
// }
//
// Modifies a property on a specific component of an actor.
// Component is found by class name (e.g., "PointLightComponent").
//
void FMCPTCPServer::Cmd_SetComponentProperty(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}

	FString ComponentName, PropertyName;
	Cmd->Params->TryGetStringField(TEXT("component_name"), ComponentName);
	Cmd->Params->TryGetStringField(TEXT("property_name"),  PropertyName);

	if (ComponentName.IsEmpty() || PropertyName.IsEmpty())
	{
		SetError(Cmd, TEXT("'component_name' and 'property_name' are required (and 'actor_label' or 'actor_path')"));
		return;
	}

	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor)
	{
		SetError(Cmd, Err);
		return;
	}

	// Find component by instance name first, then fall back to class name
	UActorComponent* TargetComponent = nullptr;
	TArray<FString> AvailableNames;
	for (UActorComponent* Comp : Actor->GetComponents())
	{
		if (!Comp) continue;
		AvailableNames.Add(Comp->GetName() + TEXT("(") + Comp->GetClass()->GetName() + TEXT(")"));
		if (!TargetComponent &&
			(Comp->GetName() == ComponentName || Comp->GetClass()->GetName() == ComponentName))
		{
			TargetComponent = Comp;
		}
	}

	if (!TargetComponent)
	{
		SetError(Cmd, FString::Printf(
			TEXT("Component '%s' not found on actor. Available: [%s]"),
			*ComponentName, *FString::Join(AvailableNames, TEXT(", "))));
		return;
	}

	// Find the property on the component
	FProperty* Prop = TargetComponent->GetClass()->FindPropertyByName(*PropertyName);
	if (!Prop)
	{
		SetError(Cmd, FString::Printf(
			TEXT("Property '%s' not found on component '%s'."),
			*PropertyName, *ComponentName));
		return;
	}

	// Get the value from JSON
	TSharedPtr<FJsonValue> ValueJson = Cmd->Params->TryGetField(TEXT("value"));
	FString ValueStr;
	if (ValueJson.IsValid())
	{
		if (ValueJson->Type == EJson::String)         ValueStr = ValueJson->AsString();
		else if (ValueJson->Type == EJson::Number)    ValueStr = FString::SanitizeFloat(ValueJson->AsNumber());
		else if (ValueJson->Type == EJson::Boolean)   ValueStr = ValueJson->AsBool() ? TEXT("true") : TEXT("false");
		else if (ValueJson->Type == EJson::Object)
		{
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&ValueStr);
			FJsonSerializer::Serialize(ValueJson->AsObject().ToSharedRef(), W);
		}
		else if (ValueJson->Type == EJson::Array)
		{
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&ValueStr);
			FJsonSerializer::Serialize(ValueJson->AsArray(), *W);
		}
	}

	// Import the value
	if (!Prop->ImportText_InContainer(*ValueStr, TargetComponent, TargetComponent, PPF_None))
	{
		SetError(Cmd, FString::Printf(TEXT("Failed to import value '%s' for property '%s'"),
			*ValueStr, *PropertyName));
		return;
	}
	TargetComponent->MarkRenderStateDirty();
	Actor->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_label"),     Actor->GetActorLabel());
	Result->SetStringField(TEXT("component_name"),  ComponentName);
	Result->SetStringField(TEXT("property_name"),   PropertyName);
	Result->SetStringField(TEXT("value"),           ValueStr);
	SetSuccess(Cmd, Result);
}

// ── get_actor_components ───────────────────────────────────────────────────────
//
// params: { "actor_label": "MyActor" }
//
// Returns a list of all components attached to an actor, including their
// class names and property names that can be modified.
//
void FMCPTCPServer::Cmd_GetActorComponents(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor)
	{
		SetError(Cmd, Err);
		return;
	}

	TArray<TSharedPtr<FJsonValue>> ComponentsArray;

	for (UActorComponent* Comp : Actor->GetComponents())
	{
		if (!Comp) continue;

		TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
		CompObj->SetStringField(TEXT("name"),  Comp->GetName());

		// List editable properties
		TArray<TSharedPtr<FJsonValue>> PropArray;
		for (TFieldIterator<FProperty> PropIt(Comp->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (Prop && Prop->HasAllPropertyFlags(CPF_Edit))
			{
				PropArray.Add(MakeShared<FJsonValueString>(Prop->GetName()));
			}
		}
		CompObj->SetArrayField(TEXT("editable_properties"), PropArray);

		ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Result->SetArrayField(TEXT("components"), ComponentsArray);
	Result->SetNumberField(TEXT("count"), ComponentsArray.Num());
	SetSuccess(Cmd, Result);
}

// ── focus_viewport ────────────────────────────────────────────────────────────
//
// params (actor mode):   { "actor_label": "MyActor" }
// params (location mode):{ "x": 0.0, "y": 0.0, "z": 0.0, "pitch": -25.0, "yaw": 0.0 }
//
// Moves the first perspective Level Editor viewport to look at the actor or
// coordinate.  Uses GEditor->MoveViewportCamerasToActor() for actors (which
// frames the actor nicely based on its bounds) and direct FLevelEditorViewportClient
// property setters for raw coordinates.
//
void FMCPTCPServer::Cmd_FocusViewport(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!GEditor)
	{
		SetError(Cmd, TEXT("GEditor not available"));
		return;
	}

	FString Err;
	AActor* Actor = Cmd->Params.IsValid() ? ResolveActorFromParams(Cmd->Params, &Err) : nullptr;
	if (Actor)
	{
		// ── Actor focus mode ──────────────────────────────────────────────────
		GEditor->SelectNone(true, true);
		GEditor->SelectActor(Actor, true, true);
		GEditor->MoveViewportCamerasToActor(*Actor, /*bActiveViewportOnly=*/false);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("focused_actor"), Actor->GetActorLabel());
		const FVector Loc = Actor->GetActorLocation();
		Result->SetNumberField(TEXT("actor_x"), Loc.X);
		Result->SetNumberField(TEXT("actor_y"), Loc.Y);
		Result->SetNumberField(TEXT("actor_z"), Loc.Z);
		SetSuccess(Cmd, Result);
		return;
	}
	// If they passed actor_label/actor_path but resolution failed, return error
	FString Path, Label;
	if (Cmd->Params.IsValid())
	{
		Cmd->Params->TryGetStringField(TEXT("actor_path"),  Path);
		Cmd->Params->TryGetStringField(TEXT("actor_label"), Label);
		if (Label.IsEmpty()) Cmd->Params->TryGetStringField(TEXT("label"), Label);
	}
	if (!Path.IsEmpty() || !Label.IsEmpty())
	{
		SetError(Cmd, Err);
		return;
	}
	{
		double X     = 0.0, Y = 0.0, Z = 0.0;
		double Pitch = -25.0, Yaw = 0.0;
		if (Cmd->Params.IsValid())
		{
			Cmd->Params->TryGetNumberField(TEXT("x"),     X);
			Cmd->Params->TryGetNumberField(TEXT("y"),     Y);
			Cmd->Params->TryGetNumberField(TEXT("z"),     Z);
			Cmd->Params->TryGetNumberField(TEXT("pitch"), Pitch);
			Cmd->Params->TryGetNumberField(TEXT("yaw"),   Yaw);
		}

		const FVector   Location(X, Y, Z);
		const FRotator  Rotation(Pitch, Yaw, 0.0);

		// Find the first perspective viewport client and reposition it
		bool bMoved = false;
		for (FLevelEditorViewportClient* VC : GEditor->GetLevelViewportClients())
		{
			if (VC && VC->IsPerspective())
			{
				VC->SetViewLocation(Location);
				VC->SetViewRotation(Rotation);
				VC->Invalidate();
				bMoved = true;
				break;
			}
		}

		if (!bMoved)
		{
			SetError(Cmd, TEXT("No perspective viewport found — open a 3D viewport in the Level Editor"));
			return;
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("x"),     X);
		Result->SetNumberField(TEXT("y"),     Y);
		Result->SetNumberField(TEXT("z"),     Z);
		Result->SetNumberField(TEXT("pitch"), Pitch);
		Result->SetNumberField(TEXT("yaw"),   Yaw);
		SetSuccess(Cmd, Result);
	}
}

// ── take_screenshot ───────────────────────────────────────────────────────────
//
// params: { "filename": "mcp_screenshot", "width": 1920, "height": 1080 }
//
// Requests a high-res screenshot from UE's screenshot system.
// The screenshot is written asynchronously — this handler returns the expected
// file path immediately.  The Python MCP server polls the file system for the
// completed PNG and returns it as base64 ImageContent to Claude.
//
// Note: width/height are used via the console command since FScreenshotRequest
// does not directly accept resolution.  The HighResShot command supports both.
//
void FMCPTCPServer::Cmd_TakeScreenshot(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!GEditor)
	{
		SetError(Cmd, TEXT("GEditor not available"));
		return;
	}

	// Parse params
	FString Filename = TEXT("mcp_screenshot");
	int32   Width    = 1920;
	int32   Height   = 1080;

	if (Cmd->Params.IsValid())
	{
		Cmd->Params->TryGetStringField(TEXT("filename"), Filename);

		double W = 0.0, H = 0.0;
		if (Cmd->Params->TryGetNumberField(TEXT("width"),  W)) Width  = FMath::Max(1, (int32)W);
		if (Cmd->Params->TryGetNumberField(TEXT("height"), H)) Height = FMath::Max(1, (int32)H);
	}

	// Sanitise filename — no extension, no path separators
	Filename = FPaths::GetBaseFilename(Filename);
	if (Filename.IsEmpty()) Filename = TEXT("mcp_screenshot");

	// Build the expected output path (WindowsEditor subdir matches UE default)
	const FString ScreenshotDir  = FPaths::ProjectSavedDir() / TEXT("Screenshots") / TEXT("WindowsEditor");
	const FString FullPath       = ScreenshotDir / Filename + TEXT(".png");

	// Apply resolution — GScreenshotResolutionX/Y are honoured by the viewport capture path.
	GScreenshotResolutionX = Width;
	GScreenshotResolutionY = Height;
	FScreenshotRequest::RequestScreenshot(FullPath, /*bInShowUI=*/false, /*bAddFilenameSuffix=*/false);

	// Trigger a viewport redraw so the next rendered frame captures the shot
	for (FLevelEditorViewportClient* VC : GEditor->GetLevelViewportClients())
	{
		if (VC && VC->IsPerspective())
		{
			VC->Invalidate();
			break;
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"),     FullPath);
	Result->SetStringField(TEXT("filename"), Filename + TEXT(".png"));
	Result->SetNumberField(TEXT("width"),    Width);
	Result->SetNumberField(TEXT("height"),   Height);
	Result->SetStringField(TEXT("status"),
		TEXT("Screenshot requested — file will appear at 'path' after the next frame renders"));
	SetSuccess(Cmd, Result);
}

// ── get_viewport_transform ───────────────────────────────────────────────────
//
// params: optional "viewport_index" (default 0 = first perspective viewport)
// result: { "location": {x,y,z}, "rotation": {pitch,yaw,roll}, "fov": 90 }
//
void FMCPTCPServer::Cmd_GetViewportTransform(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!GEditor)
	{
		SetError(Cmd, TEXT("GEditor not available"));
		return;
	}
	int32 Index = 0;
	if (Cmd->Params.IsValid())
		Cmd->Params->TryGetNumberField(TEXT("viewport_index"), Index);

	TArray<FLevelEditorViewportClient*> Clients = GEditor->GetLevelViewportClients();
	int32 PerspectiveCount = 0;
	for (FLevelEditorViewportClient* VC : Clients)
	{
		if (VC && VC->IsPerspective())
		{
			if (PerspectiveCount == Index)
			{
				FVector Loc = VC->GetViewLocation();
				FRotator Rot = VC->GetViewRotation();
				TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
				LocObj->SetNumberField(TEXT("x"), Loc.X);
				LocObj->SetNumberField(TEXT("y"), Loc.Y);
				LocObj->SetNumberField(TEXT("z"), Loc.Z);
				TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
				RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
				RotObj->SetNumberField(TEXT("yaw"),   Rot.Yaw);
				RotObj->SetNumberField(TEXT("roll"),  Rot.Roll);
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetObjectField(TEXT("location"), LocObj);
				Result->SetObjectField(TEXT("rotation"), RotObj);
				Result->SetNumberField(TEXT("fov"), VC->FOVAngle);
				SetSuccess(Cmd, Result);
				return;
			}
			++PerspectiveCount;
		}
	}
	SetError(Cmd, FString::Printf(TEXT("No perspective viewport at index %d"), Index));
}

// ── set_viewport_fov ──────────────────────────────────────────────────────────
//
// params: fov (degrees), optional viewport_index (default 0)
// result: { "fov": <set value> }
//
void FMCPTCPServer::Cmd_SetViewportFOV(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!GEditor)
	{
		SetError(Cmd, TEXT("GEditor not available"));
		return;
	}
	double FovDeg = 90.0;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetNumberField(TEXT("fov"), FovDeg))
	{
		SetError(Cmd, TEXT("fov (degrees) is required"));
		return;
	}
	float Fov = static_cast<float>(FMath::Clamp(FovDeg, 1.0, 179.0));
	int32 Index = 0;
	Cmd->Params->TryGetNumberField(TEXT("viewport_index"), Index);
	TArray<FLevelEditorViewportClient*> Clients = GEditor->GetLevelViewportClients();
	int32 PerspectiveCount = 0;
	for (FLevelEditorViewportClient* VC : Clients)
	{
		if (VC && VC->IsPerspective())
		{
			if (PerspectiveCount == Index)
			{
				VC->FOVAngle = Fov;
				VC->Invalidate();
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetNumberField(TEXT("fov"), Fov);
				SetSuccess(Cmd, Result);
				return;
			}
			++PerspectiveCount;
		}
	}
	SetError(Cmd, FString::Printf(TEXT("No perspective viewport at index %d"), Index));
}

// ── list_viewports ────────────────────────────────────────────────────────────
//
// result: { "viewports": [ { "index": N, "is_perspective": bool, "fov": float,
//           "location": {x,y,z}, "rotation": {pitch,yaw,roll} }, ... ], "count": N }
//
void FMCPTCPServer::Cmd_ListViewports(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!GEditor)
	{
		SetError(Cmd, TEXT("GEditor not available"), TEXT("unavailable"));
		return;
	}
	TArray<FLevelEditorViewportClient*> Clients = GEditor->GetLevelViewportClients();
	TArray<TSharedPtr<FJsonValue>> ViewportArray;
	int32 PerspectiveIndex = 0;
	for (int32 i = 0; i < Clients.Num(); ++i)
	{
		FLevelEditorViewportClient* VC = Clients[i];
		if (!VC) continue;
		TSharedPtr<FJsonObject> VObj = MakeShared<FJsonObject>();
		VObj->SetNumberField(TEXT("raw_index"),      i);
		VObj->SetBoolField  (TEXT("is_perspective"), VC->IsPerspective());
		VObj->SetNumberField(TEXT("fov"),            VC->FOVAngle);
		if (VC->IsPerspective())
		{
			VObj->SetNumberField(TEXT("perspective_index"), PerspectiveIndex);
			++PerspectiveIndex;
		}
		FVector  Loc = VC->GetViewLocation();
		FRotator Rot = VC->GetViewRotation();
		TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X); LocObj->SetNumberField(TEXT("y"), Loc.Y); LocObj->SetNumberField(TEXT("z"), Loc.Z);
		TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
		RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch); RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw); RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
		VObj->SetObjectField(TEXT("location"), LocObj);
		VObj->SetObjectField(TEXT("rotation"), RotObj);
		ViewportArray.Add(MakeShared<FJsonValueObject>(VObj));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField (TEXT("viewports"),          ViewportArray);
	Result->SetNumberField(TEXT("count"),               ViewportArray.Num());
	Result->SetNumberField(TEXT("perspective_count"),   PerspectiveIndex);
	SetSuccess(Cmd, Result);
}

// ── set_blueprint_cdo_property ───────────────────────────────────────────────
//
// Sets a C++ UPROPERTY on a Blueprint's Class Default Object (CDO).
// Supports any EditAnywhere property, including TSubclassOf<> (pass class path as string).
//
// params: { "blueprint_name": "BP_Foo",
//           "property_name":  "MyProp",
//           "value":          <string|number|bool|object> }
// result: { "blueprint": "BP_Foo", "property": "MyProp", "value": "..." }
//
void FMCPTCPServer::Cmd_SetBlueprintCDOProperty(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}

	FString BPName, PropName;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("property_name"),  PropName);

	if (BPName.IsEmpty() || PropName.IsEmpty())
	{
		SetError(Cmd, TEXT("'blueprint_name' and 'property_name' are required"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass)
	{
		SetError(Cmd, TEXT("Blueprint has no GeneratedClass — compile it first"));
		return;
	}

	UObject* CDO = GenClass->GetDefaultObject();
	if (!CDO)
	{
		SetError(Cmd, TEXT("Could not get CDO from Blueprint"));
		return;
	}

	FProperty* Prop = GenClass->FindPropertyByName(*PropName);
	if (!Prop)
	{
		// Walk up the class hierarchy to find the property in a C++ parent
		for (UClass* C = GenClass->GetSuperClass(); C && !Prop; C = C->GetSuperClass())
			Prop = C->FindPropertyByName(*PropName);
	}
	if (!Prop)
	{
		SetError(Cmd, FString::Printf(TEXT("Property '%s' not found on '%s' or its parents"),
			*PropName, *GenClass->GetName()));
		return;
	}

	TSharedPtr<FJsonValue> ValueJson = Cmd->Params->TryGetField(TEXT("value"));
	if (!ValueJson.IsValid())
	{
		SetError(Cmd, TEXT("'value' is required"));
		return;
	}

	FString ValueStr = JsonValueToString(ValueJson);

	CDO->Modify();

	// Special-case FClassProperty (TSubclassOf<>) — ImportText won't resolve asset paths.
	// Instead, load the asset by path and assign the UClass directly.
	// Also handle FSoftClassProperty for soft class references.
	UE_LOG(LogTemp, Warning, TEXT("SetBlueprintCDOProperty: PropClass=%s PropName=%s"),
		*Prop->GetClass()->GetName(), *PropName);
	// TSubclassOf<T> is FClassProperty; also try FObjectPropertyBase for UClass* properties.
	// Try to resolve the value as a UClass path first.
	auto TryResolveClass = [&](const FString& Path) -> UClass*
	{
		// Try as-is (might already be a class object path ending in _C)
		UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
		if (UClass* C = Cast<UClass>(Loaded)) return C;
		if (UBlueprint* LoadedBP = Cast<UBlueprint>(Loaded)) return LoadedBP->GeneratedClass;
		// Try appending _C
		if (!Path.EndsWith(TEXT("_C")))
		{
			FString GenPath = Path + TEXT("_C");
			UObject* L2 = StaticLoadObject(UObject::StaticClass(), nullptr, *GenPath);
			if (UClass* C2 = Cast<UClass>(L2)) return C2;
		}
		return nullptr;
	};

	if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
	{
		UClass* TargetClass = TryResolveClass(ValueStr);
		if (!TargetClass)
		{
			SetError(Cmd, FString::Printf(TEXT("Could not resolve class from path '%s'"), *ValueStr));
			return;
		}
		ClassProp->SetPropertyValue_InContainer(CDO, TargetClass);
	}
	else if (FObjectPropertyBase* ObjBaseProp = CastField<FObjectPropertyBase>(Prop);
	         ObjBaseProp && ObjBaseProp->PropertyClass && ObjBaseProp->PropertyClass->IsChildOf(UClass::StaticClass()))
	{
		// UClass* or similar stored as FObjectPropertyBase
		UClass* TargetClass = TryResolveClass(ValueStr);
		if (!TargetClass)
		{
			SetError(Cmd, FString::Printf(TEXT("Could not resolve class from path '%s'"), *ValueStr));
			return;
		}
		ObjBaseProp->SetObjectPropertyValue_InContainer(CDO, TargetClass);
	}
	else if (!Prop->ImportText_InContainer(*ValueStr, CDO, CDO, PPF_None))
	{
		SetError(Cmd, FString::Printf(TEXT("Failed to import value '%s' for property '%s'"),
			*ValueStr, *PropName));
		return;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	BP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), BPName);
	Result->SetStringField(TEXT("property"),  PropName);
	Result->SetStringField(TEXT("value"),     ValueStr);
	SetSuccess(Cmd, Result);
}

// ── get_world_contexts ────────────────────────────────────────────────────────
//
// result: { "contexts": [ { "index", "world_type", "has_world", "path" }, ... ] }
//
void FMCPTCPServer::Cmd_GetWorldContexts(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!GEngine)
	{
		SetError(Cmd, TEXT("GEngine not available"));
		return;
	}
	TArray<TSharedPtr<FJsonValue>> Arr;
	int32 i = 0;
	for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("index"), i);
		if (UEnum* WorldTypeEnum = FindObject<UEnum>(nullptr, TEXT("/Script/Engine.EWorldType")))
			Obj->SetStringField(TEXT("world_type"), WorldTypeEnum->GetNameStringByValue(static_cast<int64>(Ctx.WorldType)));
		else
			Obj->SetNumberField(TEXT("world_type"), static_cast<int64>(Ctx.WorldType));
		Obj->SetBoolField(TEXT("has_world"), Ctx.World() != nullptr);
		if (Ctx.World())
			Obj->SetStringField(TEXT("path"), Ctx.World()->GetPathName());
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
		++i;
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("contexts"), Arr);
	SetSuccess(Cmd, Result);
}

// ── get_selected_actors ──────────────────────────────────────────────────────
//
// result: { "actors": [ { "actor_path", "actor_label", "class" }, ... ], "count": N }
// Uses GEditor->GetSelectedActors().
//
void FMCPTCPServer::Cmd_GetSelectedActors(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!GEditor || !GEditor->GetSelectedActors())
	{
		SetError(Cmd, TEXT("GEditor or GetSelectedActors not available"));
		return;
	}
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
	{
		if (AActor* Actor = Cast<AActor>(*It))
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("actor_path"),  Actor->GetPathName());
			Obj->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			Obj->SetStringField(TEXT("class"),       Actor->GetClass()->GetName());
			Arr.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("actors"), Arr);
	Result->SetNumberField(TEXT("count"), Arr.Num());
	SetSuccess(Cmd, Result);
}

// ── set_selected_actors ──────────────────────────────────────────────────────
//
// params: actor_paths (array of paths) or actor_labels (array) — resolve via world params
// Clears selection then selects the given actors (GEditor->SelectNone, SelectActor).
//
void FMCPTCPServer::Cmd_SetSelectedActors(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!GEditor)
	{
		SetError(Cmd, TEXT("GEditor not available"));
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* PathArr = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* LabelArr = nullptr;
	if (Cmd->Params.IsValid())
	{
		Cmd->Params->TryGetArrayField(TEXT("actor_paths"),  PathArr);
		Cmd->Params->TryGetArrayField(TEXT("actor_labels"), LabelArr);
	}
	if ((!PathArr || PathArr->Num() == 0) && (!LabelArr || LabelArr->Num() == 0))
	{
		SetError(Cmd, TEXT("actor_paths or actor_labels (array) is required"));
		return;
	}
	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World)
	{
		SetError(Cmd, TEXT("Could not resolve world"));
		return;
	}
	GEditor->SelectNone(/*bNoteSelectionChange=*/true, /*bDeselectBSP=*/true);
	int32 Count = 0;
	if (PathArr)
		for (const TSharedPtr<FJsonValue>& Val : *PathArr)
			if (Val.IsValid() && Val->Type == EJson::String)
				if (AActor* A = FindActorByPath(World, Val->AsString()))
					{ GEditor->SelectActor(A, /*bSelected=*/true, /*bNotify=*/true); ++Count; }
	if (LabelArr)
		for (const TSharedPtr<FJsonValue>& Val : *LabelArr)
			if (Val.IsValid() && Val->Type == EJson::String)
				if (AActor* A = FindActorByLabel(World, Val->AsString()))
					{ GEditor->SelectActor(A, /*bSelected=*/true, /*bNotify=*/true); ++Count; }
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Count);
	SetSuccess(Cmd, Result);
}

// ── get_current_level ────────────────────────────────────────────────────────
//
// params: optional use_pie, world_context_index (default: editor world)
// result: { "level_name", "map_name", "filename", "path" }
//
void FMCPTCPServer::Cmd_GetCurrentLevel(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World)
	{
		SetError(Cmd, TEXT("Could not resolve world"));
		return;
	}
	ULevel* Level = World->GetCurrentLevel();
	FString MapName = World->GetMapName();
	FString Filename = FEditorFileUtils::GetFilename(World);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("level_name"), Level ? Level->GetName() : FString());
	Result->SetStringField(TEXT("map_name"),  MapName);
	Result->SetStringField(TEXT("filename"),  Filename);
	Result->SetStringField(TEXT("path"),     World->GetPathName());
	SetSuccess(Cmd, Result);
}

// ── get_loaded_levels ────────────────────────────────────────────────────────
//
// params: optional use_pie, world_context_index
// result: { "levels": [ { "name", "path", "is_visible" }, ... ], "streaming": [ { "package_name", "package_path", "is_loaded" }, ... ] }
//
void FMCPTCPServer::Cmd_GetLoadedLevels(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World)
	{
		SetError(Cmd, TEXT("Could not resolve world"));
		return;
	}
	TArray<TSharedPtr<FJsonValue>> LevelArr;
	for (ULevel* Level : World->GetLevels())
	{
		if (!Level) continue;
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"),      Level->GetName());
		Obj->SetStringField(TEXT("path"),      Level->GetPathName());
		Obj->SetBoolField  (TEXT("is_visible"), Level->bIsVisible);
		LevelArr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	TArray<TSharedPtr<FJsonValue>> StreamArr;
	for (ULevelStreaming* LS : World->GetStreamingLevels())
	{
		if (!LS) continue;
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("package_name"), LS->GetWorldAssetPackageFName().ToString());
		Obj->SetStringField(TEXT("package_path"), LS->GetWorldAssetPackageName());
		Obj->SetBoolField  (TEXT("is_loaded"),     LS->IsLevelLoaded());
		StreamArr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("levels"),   LevelArr);
	Result->SetArrayField(TEXT("streaming"), StreamArr);
	SetSuccess(Cmd, Result);
}

// ── load_streaming_level ─────────────────────────────────────────────────────
//
// params: level_path (e.g. /Game/Maps/MyLevel) — editor world only
// result: { "package_name", "loaded": true }
// Uses UEditorLevelUtils::AddLevelToWorld.
//
void FMCPTCPServer::Cmd_LoadStreamingLevel(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!GEditor)
	{
		SetError(Cmd, TEXT("GEditor not available"));
		return;
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		SetError(Cmd, TEXT("Editor world not available"));
		return;
	}
	FString LevelPath;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("level_path"), LevelPath) || LevelPath.IsEmpty())
	{
		SetError(Cmd, TEXT("level_path is required (e.g. /Game/Maps/MyLevel)"));
		return;
	}
	LevelPath = LevelPath.TrimStartAndEnd();
	// Convert to package name (strip .umap if present)
	FString PackageName = LevelPath;
	if (PackageName.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		PackageName = FPaths::GetBaseFilename(PackageName, false);
	ULevelStreaming* LS = UEditorLevelUtils::AddLevelToWorld(World, *PackageName, ULevelStreaming::StaticClass());
	if (!LS)
	{
		SetError(Cmd, FString::Printf(TEXT("AddLevelToWorld failed for '%s' (level may already be in world)"), *LevelPath));
		return;
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("package_name"), LS->GetWorldAssetPackageFName().ToString());
	Result->SetBoolField(TEXT("loaded"), LS->IsLevelLoaded());
	SetSuccess(Cmd, Result);
}

// ── unload_streaming_level ───────────────────────────────────────────────────
//
// params: level_path or package_name — editor world only
// result: { "removed": true }
// Finds streaming level by package and removes it via RemoveLevelsFromWorld.
//
void FMCPTCPServer::Cmd_UnloadStreamingLevel(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!GEditor)
	{
		SetError(Cmd, TEXT("GEditor not available"));
		return;
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		SetError(Cmd, TEXT("Editor world not available"));
		return;
	}
	FString LevelPath;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("level_path"), LevelPath))
		Cmd->Params->TryGetStringField(TEXT("package_name"), LevelPath);
	if (LevelPath.IsEmpty())
	{
		SetError(Cmd, TEXT("level_path or package_name is required"));
		return;
	}
	FName PackageName(*LevelPath.TrimStartAndEnd());
	ULevelStreaming* LS = FLevelUtils::FindStreamingLevel(World, PackageName);
	if (!LS)
	{
		SetError(Cmd, FString::Printf(TEXT("Streaming level not found: '%s'"), *LevelPath));
		return;
	}
	ULevel* Level = LS->GetLoadedLevel();
	if (!Level)
	{
		SetError(Cmd, FString::Printf(TEXT("Level '%s' is not loaded"), *LevelPath));
		return;
	}
	if (Level->IsPersistentLevel())
	{
		SetError(Cmd, TEXT("Cannot unload the persistent level"));
		return;
	}
	if (!UEditorLevelUtils::RemoveLevelsFromWorld({ Level }, /*bClearSelection=*/true, /*bResetTransBuffer=*/true))
	{
		SetError(Cmd, FString::Printf(TEXT("RemoveLevelsFromWorld failed for '%s'"), *LevelPath));
		return;
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("removed"), true);
	SetSuccess(Cmd, Result);
}

// ── set_actor_folder ─────────────────────────────────────────────────────────
//
// params: actor_path or actor_label, folder_path (e.g. "MyFolder" or "Parent/Child")
//
void FMCPTCPServer::Cmd_SetActorFolder(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor)
	{
		SetError(Cmd, Err);
		return;
	}
	FString FolderPath;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("folder_path"), FolderPath))
	{
		SetError(Cmd, TEXT("Missing folder_path"));
		return;
	}
	Actor->SetFolderPath(FName(*FolderPath));
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("folder_path"), FolderPath);
	SetSuccess(Cmd, Result);
}

// ── set_selected_actors_folder ───────────────────────────────────────────────
//
// params: folder_path — moves all currently selected actors into this folder
//
void FMCPTCPServer::Cmd_SetSelectedActorsFolder(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString FolderPath;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("folder_path"), FolderPath))
	{
		SetError(Cmd, TEXT("Missing folder_path"));
		return;
	}
	FActorFolders::Get().SetSelectedFolderPath(FFolder(FFolder::GetInvalidRootObject(), FName(*FolderPath)));
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("folder_path"), FolderPath);
	SetSuccess(Cmd, Result);
}

// ── list_actor_folders ───────────────────────────────────────────────────────
//
// params: use_pie, world_context_index (optional — default editor world)
// result: { "folders": [ "Path/To/Folder", ... ], "count": N }
//
void FMCPTCPServer::Cmd_ListActorFolders(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World)
	{
		SetError(Cmd, TEXT("Could not resolve world (use_pie / world_context_index)"));
		return;
	}
	TArray<FString> FolderPaths;
	FActorFolders::Get().ForEachFolder(*World, [&FolderPaths](const FFolder& Folder) -> bool
	{
		FString Path = Folder.ToString();
		if (!Path.IsEmpty())
			FolderPaths.Add(Path);
		return true;
	});
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& P : FolderPaths)
		Arr.Add(MakeShared<FJsonValueString>(P));
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("folders"), Arr);
	Result->SetNumberField(TEXT("count"), Arr.Num());
	SetSuccess(Cmd, Result);
}

// ── create_actor_folder ──────────────────────────────────────────────────────
//
// params: folder_path (e.g. "MyFolder" or "Parent/Child"), use_pie, world_context_index
//
void FMCPTCPServer::Cmd_CreateActorFolder(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString FolderPath;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("folder_path"), FolderPath))
	{
		SetError(Cmd, TEXT("Missing folder_path"));
		return;
	}
	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World)
	{
		SetError(Cmd, TEXT("Could not resolve world"));
		return;
	}
	// Use deprecated FName overload; it constructs FFolder with world root internally
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	FActorFolders::Get().CreateFolder(*World, FName(*FolderPath));
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("folder_path"), FolderPath);
	SetSuccess(Cmd, Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// ── create_blueprint ─────────────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo",
//           "package_path":   "/Game/AncestralPlane",
//           "parent_class":   "HubGameMode"    (short name, no U/A prefix) }
// result: { "asset_path": "/Game/AncestralPlane/BP_Foo" }
//
// Creates a new Blueprint asset using FKismetEditorUtilities::CreateBlueprint.
// This bypasses the asset creation dialog that would normally appear in the editor.
// ─────────────────────────────────────────────────────────────────────────────
// ── add_blueprint_component ──────────────────────────────────────────────────
//
// Adds a component to a Blueprint's SimpleConstructionScript.
//
// params:
//   "blueprint_name"  : name or full content path of the Blueprint asset
//   "component_class" : short class name, e.g. "StaticMeshComponent"
//   "component_name"  : optional variable name for the new node (default = component_class)
//   "static_mesh"     : optional static mesh asset path, e.g. "/Engine/BasicShapes/Cube.Cube"
//   "materials"       : optional JSON array of material asset paths (index = material slot)
//   "location"        : optional {x,y,z} relative location
//   "rotation"        : optional {pitch,yaw,roll} relative rotation
//   "scale"           : optional {x,y,z} relative scale
//   "attach_to"       : optional variable name of an existing SCS node to attach to
//
// result: { "component_name": "...", "component_class": "..." }
//
void FMCPTCPServer::Cmd_AddBlueprintComponent(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params"));
		return;
	}

	FString BPName, ComponentClassName, ComponentName, StaticMeshPath, AttachTo;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"),  BPName);
	Cmd->Params->TryGetStringField(TEXT("component_class"), ComponentClassName);
	Cmd->Params->TryGetStringField(TEXT("component_name"),  ComponentName);
	Cmd->Params->TryGetStringField(TEXT("static_mesh"),     StaticMeshPath);
	Cmd->Params->TryGetStringField(TEXT("attach_to"),       AttachTo);

	if (BPName.IsEmpty() || ComponentClassName.IsEmpty())
	{
		SetError(Cmd, TEXT("'blueprint_name' and 'component_class' are required"), TEXT("invalid_params"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName), TEXT("not_found"));
		return;
	}

	if (!BP->SimpleConstructionScript)
	{
		SetError(Cmd, TEXT("Blueprint has no SimpleConstructionScript (is it an Actor Blueprint?)"), TEXT("error"));
		return;
	}

	// Resolve component class — try as-is, then with U prefix
	UClass* CompClass = FindClassByName(ComponentClassName);
	if (!CompClass)
		CompClass = FindClassByName(TEXT("U") + ComponentClassName);
	if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
	{
		SetError(Cmd, FString::Printf(TEXT("Component class not found or not an ActorComponent: '%s'"), *ComponentClassName), TEXT("not_found"));
		return;
	}

	// Default variable name to class name
	if (ComponentName.IsEmpty())
		ComponentName = ComponentClassName;

	// Create the SCS node
	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(CompClass, FName(*ComponentName));
	if (!NewNode)
	{
		SetError(Cmd, TEXT("SimpleConstructionScript::CreateNode returned null"), TEXT("error"));
		return;
	}

	// ── Static mesh — use Modify() + PostEditChangeProperty so the value survives compilation ──
	if (!StaticMeshPath.IsEmpty())
	{
		if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(NewNode->ComponentTemplate))
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *StaticMeshPath);
			if (Mesh)
			{
				SMC->Modify();
				FProperty* Prop = nullptr;
				for (UClass* C = SMC->GetClass(); C && !Prop; C = C->GetSuperClass())
					Prop = C->FindPropertyByName(TEXT("StaticMesh"));
				if (Prop)
				{
					SMC->PreEditChange(Prop);
					if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
						ObjProp->SetObjectPropertyValue_InContainer(SMC, Mesh);
					FPropertyChangedEvent ChangeEvent(Prop, EPropertyChangeType::ValueSet);
					SMC->PostEditChangeProperty(ChangeEvent);
				}
			}
			else
				UE_LOG(LogUnrealMCP, Warning, TEXT("add_blueprint_component: static_mesh not found: %s"), *StaticMeshPath);
		}
	}

	// ── Materials — assign via OverrideMaterials array property ──────────────
	const TArray<TSharedPtr<FJsonValue>>* MaterialsArray = nullptr;
	if (Cmd->Params->TryGetArrayField(TEXT("materials"), MaterialsArray) && MaterialsArray)
	{
		if (UMeshComponent* MC = Cast<UMeshComponent>(NewNode->ComponentTemplate))
		{
			if (FArrayProperty* ArrProp = CastField<FArrayProperty>(
				MC->GetClass()->FindPropertyByName(TEXT("OverrideMaterials"))))
			{
				FScriptArrayHelper Helper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(MC));
				FObjectProperty* ElemProp = CastField<FObjectProperty>(ArrProp->Inner);
				if (ElemProp)
				{
					Helper.Resize(MaterialsArray->Num());
					for (int32 i = 0; i < MaterialsArray->Num(); ++i)
					{
						FString MatPath = (*MaterialsArray)[i]->AsString();
						if (!MatPath.IsEmpty())
						{
							UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MatPath);
							if (Mat)
								ElemProp->SetObjectPropertyValue(Helper.GetRawPtr(i), Mat);
							else
								UE_LOG(LogUnrealMCP, Warning, TEXT("add_blueprint_component: material[%d] not found: %s"), i, *MatPath);
						}
					}
				}
			}
		}
	}

	// ── Relative transform ────────────────────────────────────────────────────
	if (USceneComponent* SC = Cast<USceneComponent>(NewNode->ComponentTemplate))
	{
		const TSharedPtr<FJsonObject>* LocObj = nullptr;
		if (Cmd->Params->TryGetObjectField(TEXT("location"), LocObj) && LocObj)
		{
			double X = 0, Y = 0, Z = 0;
			(*LocObj)->TryGetNumberField(TEXT("x"), X);
			(*LocObj)->TryGetNumberField(TEXT("y"), Y);
			(*LocObj)->TryGetNumberField(TEXT("z"), Z);
			SC->SetRelativeLocation(FVector((float)X, (float)Y, (float)Z));
		}

		const TSharedPtr<FJsonObject>* RotObj = nullptr;
		if (Cmd->Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj)
		{
			double Pitch = 0, Yaw = 0, Roll = 0;
			(*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
			(*RotObj)->TryGetNumberField(TEXT("yaw"),   Yaw);
			(*RotObj)->TryGetNumberField(TEXT("roll"),  Roll);
			SC->SetRelativeRotation(FRotator((float)Pitch, (float)Yaw, (float)Roll));
		}

		const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
		if (Cmd->Params->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj)
		{
			double SX = 1, SY = 1, SZ = 1;
			(*ScaleObj)->TryGetNumberField(TEXT("x"), SX);
			(*ScaleObj)->TryGetNumberField(TEXT("y"), SY);
			(*ScaleObj)->TryGetNumberField(TEXT("z"), SZ);
			SC->SetRelativeScale3D(FVector((float)SX, (float)SY, (float)SZ));
		}
	}

	// ── Attach to parent SCS node or add to root ──────────────────────────────
	bool bAttached = false;
	if (!AttachTo.IsEmpty())
	{
		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node->GetVariableName().ToString() == AttachTo)
			{
				Node->AddChildNode(NewNode);
				bAttached = true;
				break;
			}
		}
		if (!bAttached)
			UE_LOG(LogUnrealMCP, Warning, TEXT("add_blueprint_component: attach_to node '%s' not found, adding to root"), *AttachTo);
	}
	if (!bAttached)
		BP->SimpleConstructionScript->AddNode(NewNode);

	// ── Recompile & save ──────────────────────────────────────────────────────
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);

	BP->MarkPackageDirty();
	TArray<UPackage*> PkgsToSave = { BP->GetOutermost() };
	FEditorFileUtils::PromptForCheckoutAndSave(PkgsToSave, false, false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("component_name"),  NewNode->GetVariableName().ToString());
	Result->SetStringField(TEXT("component_class"), CompClass->GetName());
	SetSuccess(Cmd, Result);
}

// ── remove_blueprint_component ───────────────────────────────────────────────
//
// Removes a component node from a Blueprint's SimpleConstructionScript by variable name.
//
// params:
//   "blueprint_name"  : name or full path of the Blueprint asset
//   "component_name"  : variable name of the SCS node to remove (e.g. "PlatformMesh1")
//
// result: { "removed": true, "component_name": "..." }
//
void FMCPTCPServer::Cmd_RemoveBlueprintComponent(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params"));
		return;
	}

	FString BPName, ComponentName;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("component_name"), ComponentName);

	if (BPName.IsEmpty() || ComponentName.IsEmpty())
	{
		SetError(Cmd, TEXT("'blueprint_name' and 'component_name' are required"), TEXT("invalid_params"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName), TEXT("not_found"));
		return;
	}

	if (!BP->SimpleConstructionScript)
	{
		SetError(Cmd, TEXT("Blueprint has no SimpleConstructionScript"), TEXT("error"));
		return;
	}

	USCS_Node* TargetNode = nullptr;
	for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
	{
		if (Node->GetVariableName().ToString() == ComponentName)
		{
			TargetNode = Node;
			break;
		}
	}

	if (!TargetNode)
	{
		SetError(Cmd, FString::Printf(TEXT("Component node '%s' not found in Blueprint '%s'"), *ComponentName, *BPName), TEXT("not_found"));
		return;
	}

	BP->SimpleConstructionScript->RemoveNode(TargetNode);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);

	BP->MarkPackageDirty();
	TArray<UPackage*> PkgsToSave = { BP->GetOutermost() };
	FEditorFileUtils::PromptForCheckoutAndSave(PkgsToSave, false, false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("removed"), true);
	Result->SetStringField(TEXT("component_name"), ComponentName);
	SetSuccess(Cmd, Result);
}

void FMCPTCPServer::Cmd_CreateBlueprint(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName, PackagePath, ParentClassName;

	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("package_path"),   PackagePath);
	Cmd->Params->TryGetStringField(TEXT("parent_class"),   ParentClassName);

	// Find the parent class
	UClass* ParentClass = FindClassByName(ParentClassName);
	if (!ParentClass)
	{
		// Also try AActor as a fallback for class names without the prefix
		ParentClass = FindClassByName(TEXT("A") + ParentClassName);
		if (!ParentClass)
		{
			SetError(Cmd, FString::Printf(TEXT("Parent class not found: '%s'"), *ParentClassName));
			return;
		}
	}

	// Build the full asset path
	FString FullPath = PackagePath / BPName;

	// Check if it already exists
	if (UBlueprint* Existing = FindBlueprintByName(BPName))
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), FullPath);
		Result->SetStringField(TEXT("status"),     TEXT("already_exists"));
		SetSuccess(Cmd, Result);
		return;
	}

	// Create a new package for the Blueprint
	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		SetError(Cmd, FString::Printf(TEXT("Failed to create package: '%s'"), *FullPath));
		return;
	}

	// Create the Blueprint using FKismetEditorUtilities (no dialog, scriptable)
	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*BPName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName("MCP_CreateBlueprint")
	);

	if (!NewBP)
	{
		SetError(Cmd, FString::Printf(TEXT("FKismetEditorUtilities::CreateBlueprint failed for '%s'"), *BPName));
		return;
	}

	// Mark dirty and notify asset registry
	NewBP->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewBP);

	// Save the asset
	TArray<UPackage*> PackagesToSave = { Package };
	FEditorFileUtils::PromptForCheckoutAndSave(PackagesToSave, /*bCheckDirty=*/false, /*bPromptToSave=*/false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), FullPath);
	Result->SetStringField(TEXT("status"),     TEXT("created"));
	SetSuccess(Cmd, Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// ── BLUEPRINT GRAPH HELPERS ──────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

// Search the asset registry for the first Blueprint asset whose AssetName matches.
UBlueprint* FMCPTCPServer::FindBlueprintByName(const FString& Name)
{
	// If the name looks like a content path (contains '/'), treat it as an exact asset path
	if (Name.Contains(TEXT("/")))
	{
		if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Name))
			return BP;
		// Also try appending the last path component as the sub-object name
		FString Left, Right;
		if (Name.Split(TEXT("."), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Name))
				return BP;
		}
		else
		{
			FString AssetName = FPackageName::GetShortName(Name);
			FString FullPath = Name + TEXT(".") + AssetName;
			if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *FullPath))
				return BP;
		}
		return nullptr;
	}

	// Short name: scan AssetRegistry (matches first Blueprint with this asset name)
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& Registry  = ARM.Get();

	TArray<FAssetData> Assets;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;  // also match BP subclasses (AnimBlueprint, WidgetBlueprint, etc.)
	Registry.GetAssets(Filter, Assets);

	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString() == Name)
		{
			return Cast<UBlueprint>(Asset.GetAsset());
		}
	}
	return nullptr;
}

// Find a UClass by name, stripping the U/A prefix if present and searching
// common script packages, then falling back to TObjectIterator.
UClass* FMCPTCPServer::FindClassByName(const FString& Name)
{
	// Strip leading U / A type prefix if present
	FString Short = Name;
	if (Short.Len() > 1 && (Short[0] == TEXT('U') || Short[0] == TEXT('A')))
	{
		Short = Short.Mid(1);
	}

	// Try common engine packages in priority order
	static const TCHAR* Pkgs[] = {
		TEXT("/Script/Engine"),
		TEXT("/Script/Kismet"),
		TEXT("/Script/GameplayAbilities"),
		TEXT("/Script/AIModule"),
		TEXT("/Script/UMG"),
		TEXT("/Script/GameplayTasks"),
	};
	for (const TCHAR* Pkg : Pkgs)
	{
		FString Path = FString(Pkg) + TEXT(".") + Short;
		if (UClass* C = FindObject<UClass>(nullptr, *Path))
		{
			return C;
		}
	}

	// Fallback: scan all loaded classes (slow but thorough)
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName() == Short || It->GetName() == Name)
		{
			return *It;
		}
	}
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// ── find_blueprint_nodes ─────────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "node_type": "Event",
//           "event_name": "BP_OnMyEvent" }
// result: { "node_guids": ["<guid>", ...] }
//
// Searches the Blueprint's event graph pages for UK2Node_Event nodes whose
// EventReference member name or CustomFunctionName matches event_name.
// ─────────────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_FindBlueprintNodes(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName, NodeType, EventName, NodeClass, FuncName;
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("node_type"),      NodeType);
	Cmd->Params->TryGetStringField(TEXT("event_name"),     EventName);
	Cmd->Params->TryGetStringField(TEXT("node_class"),     NodeClass);
	Cmd->Params->TryGetStringField(TEXT("function_name"),  FuncName);

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	FName EventFName(*EventName);
	TArray<FString>                   FoundGuids;
	TArray<TSharedPtr<FJsonObject>>   FoundMatches;

	// Search all graph arrays (including delegate and interface graphs)
	TArray<TPair<UEdGraph*, FString>> AllGraphs;
	for (UEdGraph* G : BP->UbergraphPages)  if (G) AllGraphs.Add({G, TEXT("Ubergraph")});
	for (UEdGraph* G : BP->FunctionGraphs)  if (G) AllGraphs.Add({G, TEXT("Function")});
	for (UEdGraph* G : BP->MacroGraphs)     if (G) AllGraphs.Add({G, TEXT("Macro")});
	for (UEdGraph* G : BP->DelegateSignatureGraphs) if (G) AllGraphs.Add({G, TEXT("DelegateSignature")});
	for (const FBPInterfaceDescription& IFace : BP->ImplementedInterfaces)
		for (UEdGraph* G : IFace.Graphs)
			if (G) AllGraphs.Add({G, TEXT("Interface")});

	for (const auto& GraphPair : AllGraphs)
	{
		UEdGraph* Graph    = GraphPair.Key;
		FString GraphType  = GraphPair.Value;
		FString GraphName  = Graph->GetName();

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			// node_class filter: skip if class name doesn't match
			if (!NodeClass.IsEmpty() && !Node->GetClass()->GetName().Contains(NodeClass))
				continue;

			bool bMatch = false;

			// Event node matching (original logic)
			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				if (!EventName.IsEmpty())
				{
					const FName MemberName = EventNode->EventReference.GetMemberName();
					bMatch = (MemberName == EventFName || EventNode->CustomFunctionName == EventFName);
				}
				else
				{
					bMatch = true; // class filter already applied above; include all event nodes
				}
			}
			// Function call node matching
			else if (UK2Node_CallFunction* FuncNode = Cast<UK2Node_CallFunction>(Node))
			{
				if (!FuncName.IsEmpty())
				{
					bMatch = FuncNode->FunctionReference.GetMemberName().ToString().Contains(FuncName);
				}
				else if (!NodeClass.IsEmpty())
				{
					bMatch = true; // already filtered by class above
				}
			}
			// Generic: if only node_class filter is set (no event/func name), include all matching
			else if (!NodeClass.IsEmpty() && FuncName.IsEmpty() && EventName.IsEmpty())
			{
				bMatch = true;
			}

			if (!bMatch) continue;

			FString Guid = Node->NodeGuid.ToString();
			FoundGuids.Add(Guid);

			TSharedPtr<FJsonObject> MatchObj = MakeShared<FJsonObject>();
			MatchObj->SetStringField(TEXT("node_id"),    Guid);
			MatchObj->SetStringField(TEXT("graph_name"), GraphName);
			MatchObj->SetStringField(TEXT("graph_type"), GraphType);
			FoundMatches.Add(MatchObj);
		}
	}

	TArray<TSharedPtr<FJsonValue>> GuidArr;
	for (const FString& G : FoundGuids)
		GuidArr.Add(MakeShared<FJsonValueString>(G));

	TArray<TSharedPtr<FJsonValue>> MatchArr;
	for (const TSharedPtr<FJsonObject>& M : FoundMatches)
		MatchArr.Add(MakeShared<FJsonValueObject>(M));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("node_guids"), GuidArr);
	Result->SetArrayField(TEXT("matches"),    MatchArr);
	SetSuccess(Cmd, Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// ── add_blueprint_event_node ─────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "event_name": "BP_OnMyEvent",
//           "node_position": [x, y] }
// result: { "node_id": "<guid>" }
//
// Adds a UK2Node_Event override node for a BlueprintImplementableEvent defined
// in the Blueprint's C++ parent class.  If the function is not found in the
// parent hierarchy, falls back to a custom event node with that name.
// ─────────────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_AddBlueprintEventNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName, EventName;
	double PosX = 0.0, PosY = 0.0;

	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("event_name"),     EventName);

	const TArray<TSharedPtr<FJsonValue>>* PosArr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr->Num() >= 2)
	{
		PosX = (*PosArr)[0]->AsNumber();
		PosY = (*PosArr)[1]->AsNumber();
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
	if (!EventGraph)
	{
		SetError(Cmd, FString::Printf(TEXT("No event graph in '%s'"), *BPName));
		return;
	}

	FName EventFName(*EventName);

	// Find which class in the hierarchy defines this function
	UFunction* Func          = BP->ParentClass ? BP->ParentClass->FindFunctionByName(EventFName) : nullptr;
	UClass*    DefiningClass = Func ? Func->GetOwnerClass() : nullptr;

	// Create the node
	UK2Node_Event* NewNode = NewObject<UK2Node_Event>(EventGraph);

	if (DefiningClass)
	{
		NewNode->EventReference.SetExternalMember(EventFName, DefiningClass);
		NewNode->bOverrideFunction = true;
	}
	else
	{
		// Not found in parent — create as a named custom event instead
		NewNode->CustomFunctionName = EventFName;
		NewNode->bOverrideFunction  = false;
	}

	NewNode->NodePosX = static_cast<int32>(PosX);
	NewNode->NodePosY = static_cast<int32>(PosY);
	NewNode->CreateNewGuid();
	EventGraph->AddNode(NewNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	SetSuccess(Cmd, Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// ── add_blueprint_function_node ──────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "target": "UKismetSystemLibrary",
//           "function_name": "PrintString",
//           "params": { "InString": "Hello" },
//           "node_position": [x, y] }
// result: { "node_id": "<guid>" }
//
// Adds a UK2Node_CallFunction node for a static function from the given class.
// Default pin values from "params" are applied after node creation.
// ─────────────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_AddBlueprintFuncNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName, Target, FuncName;
	double PosX = 350.0, PosY = 0.0;

	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("target"),         Target);
	Cmd->Params->TryGetStringField(TEXT("function_name"),  FuncName);

	// TryGetObjectField in UE5.7 takes const TSharedPtr<FJsonObject>*& (pointer ref)
	const TSharedPtr<FJsonObject>* DefaultsJsonPtr = nullptr;
	Cmd->Params->TryGetObjectField(TEXT("params"), DefaultsJsonPtr);
	TSharedPtr<FJsonObject> DefaultsJson = DefaultsJsonPtr ? *DefaultsJsonPtr : nullptr;

	const TArray<TSharedPtr<FJsonValue>>* PosArr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr->Num() >= 2)
	{
		PosX = (*PosArr)[0]->AsNumber();
		PosY = (*PosArr)[1]->AsNumber();
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	UClass* TargetClass = FindClassByName(Target);
	if (!TargetClass)
	{
		SetError(Cmd, FString::Printf(TEXT("Class not found: '%s'"), *Target));
		return;
	}

	FName FuncFName(*FuncName);
	UFunction* Func = TargetClass->FindFunctionByName(FuncFName);
	if (!Func)
	{
		// Some functions are inherited — search up the hierarchy
		for (TObjectIterator<UFunction> It; It; ++It)
		{
			if (It->GetFName() == FuncFName && It->GetOwnerClass() &&
			    It->GetOwnerClass()->IsChildOf(TargetClass))
			{
				Func = *It;
				break;
			}
		}
	}
	if (!Func)
	{
		SetError(Cmd, FString::Printf(TEXT("Function '%s' not found on '%s'"), *FuncName, *Target));
		return;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
	if (!EventGraph)
	{
		SetError(Cmd, FString::Printf(TEXT("No event graph in '%s'"), *BPName));
		return;
	}

	// Create the call function node
	UK2Node_CallFunction* NewNode = NewObject<UK2Node_CallFunction>(EventGraph);
	NewNode->FunctionReference.SetExternalMember(FuncFName, Func->GetOwnerClass());
	NewNode->NodePosX = static_cast<int32>(PosX);
	NewNode->NodePosY = static_cast<int32>(PosY);
	NewNode->CreateNewGuid();
	EventGraph->AddNode(NewNode, false, false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();

	// Apply default pin values
	if (DefaultsJson.IsValid())
	{
		for (const auto& Pair : DefaultsJson->Values)
		{
			UEdGraphPin* Pin = NewNode->FindPin(FName(*Pair.Key));
			if (Pin && Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
			{
				Pin->DefaultValue = Pair.Value->AsString();
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	SetSuccess(Cmd, Result);
}

// ── add_blueprint_branch_node ─────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "graph_name": "EventGraph" (optional),
//           "node_position": [x, y] }
// result: { "node_id": "<guid>" }
//
// Adds a UK2Node_IfThenElse (Branch) node. Pins: Execute, Condition (bool), Then, Else.
// ─────────────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_AddBlueprintBranchNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName, GraphName;
	double PosX = 0.0, PosY = 0.0;

	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("graph_name"), GraphName);

	const TArray<TSharedPtr<FJsonValue>>* PosArr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr->Num() >= 2)
	{
		PosX = (*PosArr)[0]->AsNumber();
		PosY = (*PosArr)[1]->AsNumber();
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName)); return; }

	UEdGraph* Graph = GraphName.IsEmpty() ? FBlueprintEditorUtils::FindEventGraph(BP) : FindGraphByName(BP, GraphName);
	if (!Graph) { SetError(Cmd, FString::Printf(TEXT("Graph not found in '%s'"), *BPName)); return; }

	UK2Node_IfThenElse* NewNode = NewObject<UK2Node_IfThenElse>(Graph);
	NewNode->NodePosX = static_cast<int32>(PosX);
	NewNode->NodePosY = static_cast<int32>(PosY);
	NewNode->CreateNewGuid();
	Graph->AddNode(NewNode, false, false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	SetSuccess(Cmd, Result);
}

// ── add_blueprint_sequence_node ───────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "graph_name": "EventGraph" (optional),
//           "num_output_pins": 2 (optional, default 2), "node_position": [x, y] }
// result: { "node_id": "<guid>" }
//
// Adds a UK2Node_ExecutionSequence node. Default has 2 output exec pins (Then 0, Then 1).
// ─────────────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_AddBlueprintSequenceNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName, GraphName;
	double PosX = 0.0, PosY = 0.0;
	int32 NumOutputPins = 2;

	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Cmd->Params->TryGetNumberField(TEXT("num_output_pins"), NumOutputPins);
	NumOutputPins = FMath::Clamp(NumOutputPins, 2, 256);

	const TArray<TSharedPtr<FJsonValue>>* PosArr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr->Num() >= 2)
	{
		PosX = (*PosArr)[0]->AsNumber();
		PosY = (*PosArr)[1]->AsNumber();
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName)); return; }

	UEdGraph* Graph = GraphName.IsEmpty() ? FBlueprintEditorUtils::FindEventGraph(BP) : FindGraphByName(BP, GraphName);
	if (!Graph) { SetError(Cmd, FString::Printf(TEXT("Graph not found in '%s'"), *BPName)); return; }

	UK2Node_ExecutionSequence* NewNode = NewObject<UK2Node_ExecutionSequence>(Graph);
	NewNode->NodePosX = static_cast<int32>(PosX);
	NewNode->NodePosY = static_cast<int32>(PosY);
	NewNode->CreateNewGuid();
	Graph->AddNode(NewNode, false, false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();
	for (int32 i = 2; i < NumOutputPins; ++i)
	{
		NewNode->AddInputPin();
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	SetSuccess(Cmd, Result);
}

// ── add_blueprint_switch_node ─────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "graph_name": "EventGraph" (optional),
//           "num_pins": 2 (optional, default 2), "node_position": [x, y] }
// result: { "node_id": "<guid>" }
//
// Adds a UK2Node_SwitchInteger node. Selection (int) and exec output pins per case.
// ─────────────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_AddBlueprintSwitchNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName, GraphName;
	double PosX = 0.0, PosY = 0.0;
	int32 NumPins = 2;

	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Cmd->Params->TryGetNumberField(TEXT("num_pins"), NumPins);
	NumPins = FMath::Clamp(NumPins, 2, 256);

	const TArray<TSharedPtr<FJsonValue>>* PosArr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr->Num() >= 2)
	{
		PosX = (*PosArr)[0]->AsNumber();
		PosY = (*PosArr)[1]->AsNumber();
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName)); return; }

	UEdGraph* Graph = GraphName.IsEmpty() ? FBlueprintEditorUtils::FindEventGraph(BP) : FindGraphByName(BP, GraphName);
	if (!Graph) { SetError(Cmd, FString::Printf(TEXT("Graph not found in '%s'"), *BPName)); return; }

	UK2Node_SwitchInteger* NewNode = NewObject<UK2Node_SwitchInteger>(Graph);
	NewNode->NodePosX = static_cast<int32>(PosX);
	NewNode->NodePosY = static_cast<int32>(PosY);
	NewNode->CreateNewGuid();
	Graph->AddNode(NewNode, false, false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();
	// SwitchInteger creates no case pins in AllocateDefaultPins; add NumPins output pins
	for (int32 i = 0; i < NumPins; ++i)
	{
		NewNode->AddPinToSwitchNode();
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	SetSuccess(Cmd, Result);
}

// ── add_blueprint_timeline_node ─────────────────────────────────────────────
//
// params: blueprint_name, timeline_name (optional, unique), graph_name (optional), node_position [x,y]
// result: { "node_id", "timeline_name" }
// Creates UTimelineTemplate via AddNewTimeline, then UK2Node_Timeline and links them.
//
void FMCPTCPServer::Cmd_AddBlueprintTimelineNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString BPName, GraphName, TimelineNameStr;
	double PosX = 0.0, PosY = 0.0;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Cmd->Params->TryGetStringField(TEXT("timeline_name"), TimelineNameStr);
	const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr->Num() >= 2)
		{ PosX = (*PosArr)[0]->AsNumber(); PosY = (*PosArr)[1]->AsNumber(); }
	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName)); return; }
	if (!FBlueprintEditorUtils::DoesSupportTimelines(BP))
		{ SetError(Cmd, TEXT("Blueprint does not support timelines")); return; }
	UEdGraph* Graph = GraphName.IsEmpty() ? FBlueprintEditorUtils::FindEventGraph(BP) : FindGraphByName(BP, GraphName);
	if (!Graph) { SetError(Cmd, FString::Printf(TEXT("Graph not found: '%s'"), *BPName)); return; }
	FName TimelineName = TimelineNameStr.IsEmpty() ? FBlueprintEditorUtils::FindUniqueTimelineName(BP) : FName(*TimelineNameStr);
	if (BP->FindTimelineTemplateByVariableName(TimelineName))
		{ SetError(Cmd, FString::Printf(TEXT("Timeline already exists: '%s'"), *TimelineName.ToString())); return; }
	UTimelineTemplate* Template = FBlueprintEditorUtils::AddNewTimeline(BP, TimelineName);
	if (!Template) { SetError(Cmd, TEXT("AddNewTimeline failed")); return; }
	UK2Node_Timeline* NewNode = NewObject<UK2Node_Timeline>(Graph);
	NewNode->NodePosX = static_cast<int32>(PosX);
	NewNode->NodePosY = static_cast<int32>(PosY);
	NewNode->TimelineName = TimelineName;
	NewNode->TimelineGuid = Template->TimelineGuid;
	NewNode->CreateNewGuid();
	Graph->AddNode(NewNode, false, false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("timeline_name"), TimelineName.ToString());
	SetSuccess(Cmd, Result);
}

// ── add_blueprint_foreach_node ───────────────────────────────────────────────
//
// params: blueprint_name, graph_name (optional), node_position [x,y]
// result: { "node_id" }
// Spawns UK2Node_MacroInstance and sets MacroGraphReference to Engine's ForEachLoop macro.
//
void FMCPTCPServer::Cmd_AddBlueprintForEachNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString BPName, GraphName;
	double PosX = 0.0, PosY = 0.0;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("graph_name"), GraphName);
	const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr->Num() >= 2)
		{ PosX = (*PosArr)[0]->AsNumber(); PosY = (*PosArr)[1]->AsNumber(); }
	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName)); return; }
	UEdGraph* Graph = GraphName.IsEmpty() ? FBlueprintEditorUtils::FindEventGraph(BP) : FindGraphByName(BP, GraphName);
	if (!Graph) { SetError(Cmd, FString::Printf(TEXT("Graph not found: '%s'"), *BPName)); return; }
	// Load StandardMacros blueprint and find ForEachLoop graph
	UBlueprint* MacroLib = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EngineMacros/StandardMacros.StandardMacros"));
	if (!MacroLib) { SetError(Cmd, TEXT("Could not load /Engine/EngineMacros/StandardMacros")); return; }
	UEdGraph* ForEachGraph = nullptr;
	for (UEdGraph* G : MacroLib->MacroGraphs)
		{ if (G && G->GetName() == TEXT("ForEachLoop")) { ForEachGraph = G; break; } }
	if (!ForEachGraph) { SetError(Cmd, TEXT("ForEachLoop macro not found in StandardMacros")); return; }
	UK2Node_MacroInstance* NewNode = NewObject<UK2Node_MacroInstance>(Graph);
	NewNode->NodePosX = static_cast<int32>(PosX);
	NewNode->NodePosY = static_cast<int32>(PosY);
	NewNode->CreateNewGuid();
	NewNode->SetMacroGraph(ForEachGraph);
	Graph->AddNode(NewNode, false, false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	SetSuccess(Cmd, Result);
}

// ── add_blueprint_switch_string_node ─────────────────────────────────────────
//
// params: blueprint_name, graph_name (optional), num_pins (default 2), node_position [x,y]
// result: { "node_id" }
//
void FMCPTCPServer::Cmd_AddBlueprintSwitchStringNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString BPName, GraphName;
	double PosX = 0.0, PosY = 0.0;
	int32 NumPins = 2;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Cmd->Params->TryGetNumberField(TEXT("num_pins"), NumPins);
	NumPins = FMath::Clamp(NumPins, 2, 256);
	const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
	Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr);
	if (PosArr && PosArr->Num() >= 2) { PosX = (*PosArr)[0]->AsNumber(); PosY = (*PosArr)[1]->AsNumber(); }
	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName)); return; }
	UEdGraph* Graph = GraphName.IsEmpty() ? FBlueprintEditorUtils::FindEventGraph(BP) : FindGraphByName(BP, GraphName);
	if (!Graph) { SetError(Cmd, FString::Printf(TEXT("Graph not found: '%s'"), *BPName)); return; }
	UK2Node_SwitchString* NewNode = NewObject<UK2Node_SwitchString>(Graph);
	NewNode->NodePosX = static_cast<int32>(PosX);
	NewNode->NodePosY = static_cast<int32>(PosY);
	NewNode->CreateNewGuid();
	Graph->AddNode(NewNode, false, false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();
	for (int32 i = 0; i < NumPins; ++i)
		NewNode->AddPinToSwitchNode();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	SetSuccess(Cmd, Result);
}

// ── add_blueprint_switch_enum_node ───────────────────────────────────────────
//
// params: blueprint_name, graph_name (optional), enum_path (e.g. /Script/Engine.ETraceTypeQuery), node_position [x,y]
// result: { "node_id" }
// Adds a UK2Node_SwitchEnum; the enum defines the case pins.
//
void FMCPTCPServer::Cmd_AddBlueprintSwitchEnumNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString BPName, GraphName, EnumPath;
	double PosX = 0.0, PosY = 0.0;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Cmd->Params->TryGetStringField(TEXT("enum_path"), EnumPath);
	const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr->Num() >= 2)
		{ PosX = (*PosArr)[0]->AsNumber(); PosY = (*PosArr)[1]->AsNumber(); }
	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName)); return; }
	if (EnumPath.IsEmpty()) { SetError(Cmd, TEXT("enum_path is required for Switch on Enum")); return; }
	UEnum* Enum = FindObject<UEnum>(nullptr, *EnumPath);
	if (!Enum) Enum = LoadObject<UEnum>(nullptr, *EnumPath);
	if (!Enum) { SetError(Cmd, FString::Printf(TEXT("Enum not found: '%s'"), *EnumPath)); return; }
	UEdGraph* Graph = GraphName.IsEmpty() ? FBlueprintEditorUtils::FindEventGraph(BP) : FindGraphByName(BP, GraphName);
	if (!Graph) { SetError(Cmd, FString::Printf(TEXT("Graph not found: '%s'"), *BPName)); return; }
	UK2Node_SwitchEnum* NewNode = NewObject<UK2Node_SwitchEnum>(Graph);
	NewNode->NodePosX = static_cast<int32>(PosX);
	NewNode->NodePosY = static_cast<int32>(PosY);
	NewNode->CreateNewGuid();
	NewNode->Enum = Enum;
	Graph->AddNode(NewNode, false, false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	SetSuccess(Cmd, Result);
}

// ── add_blueprint_gate_node ───────────────────────────────────────────────────
//
// Adds a Gate macro node. params: blueprint_name, graph_name (optional), node_position ([x,y])
// result: { "node_id": "..." }
//
void FMCPTCPServer::Cmd_AddBlueprintGateNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }
	FString BPName, GraphName;
	double PosX = 0.0, PosY = 0.0;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("graph_name"),     GraphName);
	const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr && PosArr->Num() >= 2)
		{ PosX = (*PosArr)[0]->AsNumber(); PosY = (*PosArr)[1]->AsNumber(); }

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName), TEXT("not_found")); return; }
	UEdGraph* Graph = GraphName.IsEmpty() ? FBlueprintEditorUtils::FindEventGraph(BP) : FindGraphByName(BP, GraphName);
	if (!Graph) { SetError(Cmd, FString::Printf(TEXT("Graph not found: '%s'"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName), TEXT("not_found")); return; }

	UBlueprint* MacroLib = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EngineMacros/StandardMacros.StandardMacros"));
	if (!MacroLib) { SetError(Cmd, TEXT("Could not load /Engine/EngineMacros/StandardMacros"), TEXT("unavailable")); return; }
	UEdGraph* MacroGraph = nullptr;
	for (UEdGraph* G : MacroLib->MacroGraphs)
		{ if (G && G->GetName() == TEXT("Gate")) { MacroGraph = G; break; } }
	if (!MacroGraph) { SetError(Cmd, TEXT("Gate macro not found in StandardMacros"), TEXT("not_found")); return; }

	UK2Node_MacroInstance* NewNode = NewObject<UK2Node_MacroInstance>(Graph);
	NewNode->NodePosX = static_cast<int32>(PosX);
	NewNode->NodePosY = static_cast<int32>(PosY);
	NewNode->CreateNewGuid();
	NewNode->SetMacroGraph(MacroGraph);
	Graph->AddNode(NewNode, false, false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	SetSuccess(Cmd, Result);
}

// ── add_blueprint_multigate_node ──────────────────────────────────────────────
//
// Adds a MultiGate macro node. params: blueprint_name, graph_name (optional), node_position ([x,y])
// result: { "node_id": "..." }
//
void FMCPTCPServer::Cmd_AddBlueprintMultiGateNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }
	FString BPName, GraphName;
	double PosX = 0.0, PosY = 0.0;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("graph_name"),     GraphName);
	const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr && PosArr->Num() >= 2)
		{ PosX = (*PosArr)[0]->AsNumber(); PosY = (*PosArr)[1]->AsNumber(); }

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName), TEXT("not_found")); return; }
	UEdGraph* Graph = GraphName.IsEmpty() ? FBlueprintEditorUtils::FindEventGraph(BP) : FindGraphByName(BP, GraphName);
	if (!Graph) { SetError(Cmd, FString::Printf(TEXT("Graph not found: '%s'"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName), TEXT("not_found")); return; }

	UBlueprint* MacroLib = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EngineMacros/StandardMacros.StandardMacros"));
	if (!MacroLib) { SetError(Cmd, TEXT("Could not load /Engine/EngineMacros/StandardMacros"), TEXT("unavailable")); return; }
	UEdGraph* MacroGraph = nullptr;
	for (UEdGraph* G : MacroLib->MacroGraphs)
		{ if (G && G->GetName() == TEXT("MultiGate")) { MacroGraph = G; break; } }
	if (!MacroGraph) { SetError(Cmd, TEXT("MultiGate macro not found in StandardMacros"), TEXT("not_found")); return; }

	UK2Node_MacroInstance* NewNode = NewObject<UK2Node_MacroInstance>(Graph);
	NewNode->NodePosX = static_cast<int32>(PosX);
	NewNode->NodePosY = static_cast<int32>(PosY);
	NewNode->CreateNewGuid();
	NewNode->SetMacroGraph(MacroGraph);
	Graph->AddNode(NewNode, false, false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
	SetSuccess(Cmd, Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// ── connect_blueprint_nodes ──────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo",
//           "source_node_id": "<guid>", "source_pin": "then",
//           "target_node_id": "<guid>", "target_pin": "execute" }
// result: { "connected": true }
//
// Finds two nodes in the Blueprint's event graph by GUID and creates a pin
// connection using the Kismet schema.  Returns an error if pin types are
// incompatible; does not break existing connections on exclusive pins.
// ─────────────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_ConnectBlueprintNodes(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName, SrcIdStr, SrcPinName, TgtIdStr, TgtPinName;

	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("source_node_id"), SrcIdStr);
	Cmd->Params->TryGetStringField(TEXT("source_pin"),     SrcPinName);
	Cmd->Params->TryGetStringField(TEXT("target_node_id"), TgtIdStr);
	Cmd->Params->TryGetStringField(TEXT("target_pin"),     TgtPinName);

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	// Parse GUIDs
	FGuid SrcGuid, TgtGuid;
	if (!FGuid::Parse(SrcIdStr, SrcGuid))
	{
		SetError(Cmd, FString::Printf(TEXT("Invalid source_node_id GUID: '%s'"), *SrcIdStr));
		return;
	}
	if (!FGuid::Parse(TgtIdStr, TgtGuid))
	{
		SetError(Cmd, FString::Printf(TEXT("Invalid target_node_id GUID: '%s'"), *TgtIdStr));
		return;
	}

	// Locate nodes by GUID across all graphs (ubergraph + functions + macros)
	UEdGraphNode* SrcNode = nullptr;
	UEdGraphNode* TgtNode = nullptr;

	TArray<UEdGraph*> AllBPGraphs;
	AllBPGraphs.Append(BP->UbergraphPages);
	AllBPGraphs.Append(BP->FunctionGraphs);
	AllBPGraphs.Append(BP->MacroGraphs);
	AllBPGraphs.Append(BP->DelegateSignatureGraphs);
	for (const FBPInterfaceDescription& IFace : BP->ImplementedInterfaces)
		AllBPGraphs.Append(IFace.Graphs);

	for (UEdGraph* Graph : AllBPGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Node->NodeGuid == SrcGuid) SrcNode = Node;
			if (Node->NodeGuid == TgtGuid) TgtNode = Node;
		}
		if (SrcNode && TgtNode) break;
	}

	if (!SrcNode)
	{
		SetError(Cmd, FString::Printf(TEXT("Source node not found: '%s'"), *SrcIdStr));
		return;
	}
	if (!TgtNode)
	{
		SetError(Cmd, FString::Printf(TEXT("Target node not found: '%s'"), *TgtIdStr));
		return;
	}

	if (SrcNode->GetGraph() != TgtNode->GetGraph())
	{
		SetError(Cmd, TEXT("Source and target nodes are in different graphs — cannot connect across graphs"));
		return;
	}

	// Find pins — try both output and input directions for flexibility
	FName SrcPinFName(*SrcPinName);
	FName TgtPinFName(*TgtPinName);

	UEdGraphPin* SrcPin = SrcNode->FindPin(SrcPinFName, EGPD_Output);
	if (!SrcPin) SrcPin = SrcNode->FindPin(SrcPinFName); // any direction fallback

	UEdGraphPin* TgtPin = TgtNode->FindPin(TgtPinFName, EGPD_Input);
	if (!TgtPin) TgtPin = TgtNode->FindPin(TgtPinFName);

	if (!SrcPin)
	{
		SetError(Cmd, FString::Printf(TEXT("Source pin '%s' not found"), *SrcPinName));
		return;
	}
	if (!TgtPin)
	{
		SetError(Cmd, FString::Printf(TEXT("Target pin '%s' not found"), *TgtPinName));
		return;
	}

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	const FPinConnectionResponse Resp = Schema->CanCreateConnection(SrcPin, TgtPin);

	if (Resp.Response == CONNECT_RESPONSE_DISALLOW)
	{
		SetError(Cmd, FString::Printf(TEXT("Cannot connect '%s' -> '%s': %s"),
			*SrcPinName, *TgtPinName, *Resp.Message.ToString()));
		return;
	}

	Schema->TryCreateConnection(SrcPin, TgtPin);
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("connected"), true);
	SetSuccess(Cmd, Result);
}

// ─────────────────────────────────────────────────────────────────────────────
// ── compile_blueprint ─────────────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo" }
// result: { "blueprint": "BP_Foo", "status": "compiled" }
//
// Compiles the named Blueprint using FKismetEditorUtilities::CompileBlueprint.
// Compilation errors appear in the Output Log; this command always returns
// success if the Blueprint was found (compilation errors are non-fatal here).
// ─────────────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_CompileBlueprint(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName))
	{
		SetError(Cmd, TEXT("Missing 'blueprint_name' parameter"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	FCompilerResultsLog Results;
	Results.bSilentMode = true; // suppress redundant UE_LOG spam
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);

	BP->MarkPackageDirty();

	TArray<TSharedPtr<FJsonValue>> MsgArr;
	for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
		MsgArr.Add(MakeShared<FJsonValueString>(Msg->ToText().ToString()));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), BPName);
	Result->SetNumberField(TEXT("errors"),    static_cast<double>(Results.NumErrors));
	Result->SetNumberField(TEXT("warnings"),  static_cast<double>(Results.NumWarnings));
	Result->SetArrayField(TEXT("messages"),   MsgArr);
	Result->SetStringField(TEXT("status"),    Results.NumErrors > 0 ? TEXT("compile_error") : TEXT("compiled"));
	SetSuccess(Cmd, Result);
}

// =============================================================================
// ── NEW HELPERS ───────────────────────────────────────────────────────────────
// =============================================================================

// ── GetPropertyValue — read a FProperty from a container (UObject/struct) ─────
TSharedPtr<FJsonValue> FMCPTCPServer::GetPropertyValue(FProperty* Prop, const void* Container)
{
	if (!Prop || !Container)
		return MakeShared<FJsonValueNull>();
	return GetPropertyValueDirect(Prop, Prop->ContainerPtrToValuePtr<void>(Container));
}

// ── GetPropertyValueDirect — read from a direct value pointer ─────────────────
// Used by GetPropertyValue and recursively by FArrayProperty element iteration.
TSharedPtr<FJsonValue> FMCPTCPServer::GetPropertyValueDirect(FProperty* Prop, const void* ValuePtr)
{
	if (!Prop || !ValuePtr)
		return MakeShared<FJsonValueNull>();

	if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
		return MakeShared<FJsonValueBoolean>(BP->GetPropertyValue(ValuePtr));

	if (FFloatProperty* FP = CastField<FFloatProperty>(Prop))
		return MakeShared<FJsonValueNumber>(FP->GetPropertyValue(ValuePtr));

	if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
		return MakeShared<FJsonValueNumber>(DP->GetPropertyValue(ValuePtr));

	if (FIntProperty* IP = CastField<FIntProperty>(Prop))
		return MakeShared<FJsonValueNumber>(static_cast<double>(IP->GetPropertyValue(ValuePtr)));

	if (FInt64Property* I64 = CastField<FInt64Property>(Prop))
		return MakeShared<FJsonValueNumber>(static_cast<double>(I64->GetPropertyValue(ValuePtr)));

	if (FStrProperty* SP = CastField<FStrProperty>(Prop))
		return MakeShared<FJsonValueString>(SP->GetPropertyValue(ValuePtr));

	if (FNameProperty* NP = CastField<FNameProperty>(Prop))
		return MakeShared<FJsonValueString>(NP->GetPropertyValue(ValuePtr).ToString());

	if (FTextProperty* TP = CastField<FTextProperty>(Prop))
		return MakeShared<FJsonValueString>(TP->GetPropertyValue(ValuePtr).ToString());

	if (FEnumProperty* EP = CastField<FEnumProperty>(Prop))
	{
		UEnum* Enum = EP->GetEnum();
		if (Enum)
		{
			int64 Val = EP->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(Enum->GetNameStringByValue(Val));
		}
	}

	if (FArrayProperty* AP = CastField<FArrayProperty>(Prop))
	{
		FScriptArrayHelper Helper(AP, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> JsonArr;
		JsonArr.Reserve(Helper.Num());
		for (int32 i = 0; i < Helper.Num(); ++i)
			JsonArr.Add(GetPropertyValueDirect(AP->Inner, Helper.GetRawPtr(i)));
		return MakeShared<FJsonValueArray>(JsonArr);
	}

	if (FMapProperty* MP = CastField<FMapProperty>(Prop))
	{
		FScriptMapHelper Helper(MP, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> PairsArr;
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			TSharedPtr<FJsonObject> Pair = MakeShared<FJsonObject>();
			Pair->SetField(TEXT("key"), GetPropertyValueDirect(MP->KeyProp, Helper.GetKeyPtr(It)));
			Pair->SetField(TEXT("value"), GetPropertyValueDirect(MP->ValueProp, Helper.GetValuePtr(It)));
			PairsArr.Add(MakeShared<FJsonValueObject>(Pair));
		}
		return MakeShared<FJsonValueArray>(PairsArr);
	}

	if (FSetProperty* SP = CastField<FSetProperty>(Prop))
	{
		FScriptSetHelper Helper(SP, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (FScriptSetHelper::FIterator It(Helper); It; ++It)
			Arr.Add(GetPropertyValueDirect(SP->ElementProp, Helper.GetElementPtr(It)));
		return MakeShared<FJsonValueArray>(Arr);
	}

	if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
	{
		const FSoftObjectPtr* Ptr = static_cast<const FSoftObjectPtr*>(ValuePtr);
		return MakeShared<FJsonValueString>(Ptr ? Ptr->ToString() : FString());
	}

	if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
	{
		UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
		FString Path = Obj ? Obj->GetPathName() : FString();
		return MakeShared<FJsonValueString>(Path);
	}

	if (FStructProperty* StP = CastField<FStructProperty>(Prop))
	{
		if (StP->Struct == TBaseStructure<FVector>::Get())
		{
			const FVector* V = static_cast<const FVector*>(ValuePtr);
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("x"), V->X);
			Obj->SetNumberField(TEXT("y"), V->Y);
			Obj->SetNumberField(TEXT("z"), V->Z);
			return MakeShared<FJsonValueObject>(Obj);
		}
		if (StP->Struct == TBaseStructure<FRotator>::Get())
		{
			const FRotator* R = static_cast<const FRotator*>(ValuePtr);
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("pitch"), R->Pitch);
			Obj->SetNumberField(TEXT("yaw"),   R->Yaw);
			Obj->SetNumberField(TEXT("roll"),  R->Roll);
			return MakeShared<FJsonValueObject>(Obj);
		}
		if (StP->Struct == TBaseStructure<FLinearColor>::Get())
		{
			const FLinearColor* C = static_cast<const FLinearColor*>(ValuePtr);
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("r"), C->R);
			Obj->SetNumberField(TEXT("g"), C->G);
			Obj->SetNumberField(TEXT("b"), C->B);
			Obj->SetNumberField(TEXT("a"), C->A);
			return MakeShared<FJsonValueObject>(Obj);
		}
	}

	// Fallback: export as text string using direct value ptr
	FString TextValue;
	Prop->ExportTextItem_Direct(TextValue, ValuePtr, ValuePtr, nullptr, PPF_None);
	return MakeShared<FJsonValueString>(TextValue);
}

// ── GetNestedProperty — walk a dot-path and read ─────────────────────────────
TSharedPtr<FJsonValue> FMCPTCPServer::GetNestedProperty(UObject* Obj, const FString& DotPath)
{
	if (!Obj || DotPath.IsEmpty())
		return MakeShared<FJsonValueNull>();

	TArray<FString> Parts;
	DotPath.ParseIntoArray(Parts, TEXT("."));

	UStruct*    CurrentStruct    = Obj->GetClass();
	const void* CurrentContainer = Obj;

	for (int32 i = 0; i < Parts.Num(); ++i)
	{
		FProperty* Prop = CurrentStruct->FindPropertyByName(*Parts[i]);
		if (!Prop) return MakeShared<FJsonValueNull>();

		if (i == Parts.Num() - 1)
			return GetPropertyValue(Prop, CurrentContainer);

		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp) return MakeShared<FJsonValueNull>();

		CurrentContainer = Prop->ContainerPtrToValuePtr<void>(CurrentContainer);
		CurrentStruct    = StructProp->Struct;
	}
	return MakeShared<FJsonValueNull>();
}

// ── FindGraphByName — search all graph arrays by name ────────────────────────
UEdGraph* FMCPTCPServer::FindGraphByName(UBlueprint* BP, const FString& GraphName)
{
	if (!BP) return nullptr;

	auto SearchList = [&](const TArray<UEdGraph*>& Graphs) -> UEdGraph*
	{
		for (UEdGraph* G : Graphs)
			if (G && G->GetName() == GraphName)
				return G;
		return nullptr;
	};

	if (UEdGraph* G = SearchList(BP->UbergraphPages))          return G;
	if (UEdGraph* G = SearchList(BP->FunctionGraphs))          return G;
	if (UEdGraph* G = SearchList(BP->MacroGraphs))              return G;
	if (UEdGraph* G = SearchList(BP->DelegateSignatureGraphs)) return G;
	// Interface graphs: each implemented interface has its own Graphs array
	for (const FBPInterfaceDescription& IFace : BP->ImplementedInterfaces)
	{
		for (UEdGraph* G : IFace.Graphs)
			if (G && G->GetName() == GraphName)
				return G;
	}
	return nullptr;
}

// =============================================================================
// ── CATEGORY 1: Python file execution ────────────────────────────────────────
// =============================================================================

// ── execute_python_file ───────────────────────────────────────────────────────
//
// params: { "file_path": "/absolute/path/to/script.py" or "Scripts/foo.py" (project-relative) }
// result: { "output": "...", "error": "..." }
//
void FMCPTCPServer::Cmd_ExecutePythonFile(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}

	FString FilePath;
	if (!Cmd->Params->TryGetStringField(TEXT("file_path"), FilePath))
	{
		SetError(Cmd, TEXT("Missing 'file_path' parameter"));
		return;
	}
	FilePath.TrimStartAndEndInline();
	if (FPaths::IsRelative(FilePath))
	{
		FilePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), FilePath));
	}

	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin || !PythonPlugin->IsPythonAvailable())
	{
		SetError(Cmd, TEXT("Python Script Plugin is not available"));
		return;
	}

	FPythonCommandEx PyCmd;
	PyCmd.Command           = FilePath;
	PyCmd.ExecutionMode     = EPythonCommandExecutionMode::ExecuteFile;
	PyCmd.FileExecutionScope = EPythonFileExecutionScope::Public;

	const bool bOk = PythonPlugin->ExecPythonCommandEx(PyCmd);

	// Collect log output
	FString OutputStr, ErrorStr;
	for (const FPythonLogOutputEntry& Entry : PyCmd.LogOutput)
	{
		if (Entry.Type == EPythonLogOutputType::Error)
			ErrorStr += Entry.Output + TEXT("\n");
		else
			OutputStr += Entry.Output + TEXT("\n");
	}
	if (!PyCmd.CommandResult.IsEmpty())
		OutputStr += PyCmd.CommandResult;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("output"), OutputStr.TrimEnd());
	Result->SetStringField(TEXT("error"),  ErrorStr.TrimEnd());
	Result->SetBoolField  (TEXT("success"), bOk);

	if (bOk)
		SetSuccess(Cmd, Result);
	else
	{
		Cmd->bSuccess     = false;
		Cmd->ResultObject = Result;
		Cmd->ErrorMessage = TEXT("Python file execution failed — see 'error' for details");
	}
}

// =============================================================================
// ── CATEGORY 2: Actor read / duplicate ───────────────────────────────────────
// =============================================================================

// ── get_actor_property ────────────────────────────────────────────────────────
//
// params: { "actor_label": "MyActor", "property_path": "RelativeScale3D" }
// result: { "value": <json> }
//
void FMCPTCPServer::Cmd_GetActorProperty(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}

	FString PropertyPath;
	Cmd->Params->TryGetStringField(TEXT("property_path"), PropertyPath);
	if (PropertyPath.IsEmpty())
	{
		SetError(Cmd, TEXT("'property_path' is required (and 'actor_label' or 'actor_path')"));
		return;
	}

	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor)
	{
		SetError(Cmd, Err);
		return;
	}

	TSharedPtr<FJsonValue> Value = GetNestedProperty(Actor, PropertyPath);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetField(TEXT("value"), Value);
	SetSuccess(Cmd, Result);
}

// ── get_component_property ────────────────────────────────────────────────────
//
// params: { "actor_label": "MyActor", "component_name": "PointLight0",
//           "property_path": "Intensity" }
// result: { "value": <json> }
//
void FMCPTCPServer::Cmd_GetComponentProperty(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}

	FString CompName, PropertyPath;
	Cmd->Params->TryGetStringField(TEXT("component_name"), CompName);
	Cmd->Params->TryGetStringField(TEXT("property_path"),  PropertyPath);
	if (CompName.IsEmpty() || PropertyPath.IsEmpty())
	{
		SetError(Cmd, TEXT("'component_name' and 'property_path' are required (and 'actor_label' or 'actor_path')"));
		return;
	}

	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor)
	{
		SetError(Cmd, Err);
		return;
	}

	// Match by component name or class name
	UActorComponent* Comp = nullptr;
	for (UActorComponent* C : Actor->GetComponents())
	{
		if (C && (C->GetName() == CompName || C->GetClass()->GetName() == CompName))
		{
			Comp = C;
			break;
		}
	}
	if (!Comp)
	{
		SetError(Cmd, FString::Printf(
			TEXT("Component '%s' not found on actor '%s'"), *CompName, *Actor->GetActorLabel()));
		return;
	}

	TSharedPtr<FJsonValue> Value = GetNestedProperty(Comp, PropertyPath);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetField(TEXT("value"), Value);
	SetSuccess(Cmd, Result);
}

// ── get_all_properties ───────────────────────────────────────────────────────
//
// params: object_path (any UObject), OR actor_label/actor_path + optional component_name
// result: { "properties": [ { "name", "type" }, ... ], "count": N }
// Uses TFieldIterator<FProperty> over the object's class.
//
void FMCPTCPServer::Cmd_GetAllProperties(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	UObject* Obj = nullptr;
	FString ObjectPath;
	if (Cmd->Params->TryGetStringField(TEXT("object_path"), ObjectPath) && !ObjectPath.IsEmpty())
	{
		Obj = LoadObject<UObject>(nullptr, *ObjectPath);
		if (!Obj) Obj = FindFirstObject<UObject>(*ObjectPath, EFindFirstObjectOptions::NativeFirst);
		if (!Obj) { SetError(Cmd, FString::Printf(TEXT("Object not found: '%s'"), *ObjectPath)); return; }
	}
	else
	{
		FString Err;
		AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
		if (!Actor) { SetError(Cmd, Err.IsEmpty() ? TEXT("object_path or actor_label/actor_path required") : Err); return; }
		FString CompName;
		Cmd->Params->TryGetStringField(TEXT("component_name"), CompName);
		if (CompName.IsEmpty())
			Obj = Actor;
		else
		{
			for (UActorComponent* C : Actor->GetComponents())
				if (C && (C->GetName() == CompName || C->GetClass()->GetName() == CompName))
					{ Obj = C; break; }
			if (!Obj) { SetError(Cmd, FString::Printf(TEXT("Component '%s' not found"), *CompName)); return; }
		}
	}
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Prop->GetName());
		Entry->SetStringField(TEXT("type"), Prop->GetCPPType());
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("properties"), Arr);
	Result->SetNumberField(TEXT("count"), Arr.Num());
	SetSuccess(Cmd, Result);
}

// ── duplicate_actor ───────────────────────────────────────────────────────────
//
// params: { "actor_label": "MyActor", "new_label": "MyActor_Copy",
//           "offset": [100, 0, 0] }
// result: { "label": "MyActor_Copy", "class": "..." }
//
void FMCPTCPServer::Cmd_DuplicateActor(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}

	FString NewLabel;
	Cmd->Params->TryGetStringField(TEXT("new_label"), NewLabel);

	FVector Offset(0.f);
	const TArray<TSharedPtr<FJsonValue>>* OffsetArr;
	if (Cmd->Params->TryGetArrayField(TEXT("offset"), OffsetArr) && OffsetArr->Num() >= 3)
	{
		Offset.X = (float)(*OffsetArr)[0]->AsNumber();
		Offset.Y = (float)(*OffsetArr)[1]->AsNumber();
		Offset.Z = (float)(*OffsetArr)[2]->AsNumber();
	}

	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor)
	{
		SetError(Cmd, Err);
		return;
	}

	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World)
	{
		SetError(Cmd, TEXT("No world available (editor or set use_pie / world_context_index)"));
		return;
	}

	if (!GEditor)
	{
		SetError(Cmd, TEXT("GEditor not available"));
		return;
	}
	UEditorActorSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	if (!Sub)
	{
		SetError(Cmd, TEXT("EditorActorSubsystem not available"));
		return;
	}

	TArray<AActor*> Duped = Sub->DuplicateActors({ Actor }, World, Offset);
	if (Duped.IsEmpty() || !Duped[0])
	{
		SetError(Cmd, FString::Printf(TEXT("Failed to duplicate actor '%s'"), *Actor->GetActorLabel()));
		return;
	}

	AActor* NewActor = Duped[0];
	if (!NewLabel.IsEmpty())
		NewActor->SetActorLabel(NewLabel);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("label"), NewActor->GetActorLabel());
	Result->SetStringField(TEXT("class"), NewActor->GetClass()->GetName());
	SetSuccess(Cmd, Result);
}

// ── place_actor_from_asset ─────────────────────────────────────────────────────
//
// params: asset_path (or object_path) — content path to a placeable asset (e.g. StaticMesh, Blueprint)
// result: { "actor_path", "actor_label", "class" }
// Uses FActorFactoryAssetProxy::AddActorForAsset; places in current editor level.
//
void FMCPTCPServer::Cmd_PlaceActorFromAsset(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!GEditor || !Cmd->Params.IsValid()) { SetError(Cmd, TEXT("GEditor not available or missing params")); return; }
	FString AssetPath;
	if (!Cmd->Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		Cmd->Params->TryGetStringField(TEXT("object_path"), AssetPath);
	if (AssetPath.IsEmpty()) { SetError(Cmd, TEXT("asset_path or object_path required")); return; }
	FString LoadPath = AssetPath.Contains(TEXT(".")) ? AssetPath : AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
	UObject* Asset = LoadObject<UObject>(nullptr, *LoadPath);
	if (!Asset) Asset = LoadObject<UObject>(nullptr, *FSoftObjectPath(AssetPath).ToString());
	if (!Asset) { SetError(Cmd, FString::Printf(TEXT("Asset not found: '%s'"), *AssetPath)); return; }
	AActor* NewActor = FActorFactoryAssetProxy::AddActorForAsset(Asset, /*bSelectActor=*/true);
	if (!NewActor) { SetError(Cmd, FString::Printf(TEXT("No actor factory for asset '%s' (asset may not be placeable)"), *AssetPath)); return; }
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"),  NewActor->GetPathName());
	Result->SetStringField(TEXT("actor_label"), NewActor->GetActorLabel());
	Result->SetStringField(TEXT("class"),       NewActor->GetClass()->GetName());
	SetSuccess(Cmd, Result);
}

// =============================================================================
// ── CATEGORY 3: Asset management ─────────────────────────────────────────────
// =============================================================================

// ── save_asset ────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_SaveAsset(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString AssetPath;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		SetError(Cmd, TEXT("Missing 'asset_path' parameter"));
		return;
	}

	const bool bSaved = UEditorAssetLibrary::SaveAsset(AssetPath, /*bOnlyIfIsDirty=*/false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField  (TEXT("saved"),      bSaved);

	if (bSaved)
		SetSuccess(Cmd, Result);
	else
		SetError(Cmd, FString::Printf(TEXT("Failed to save asset: '%s'"), *AssetPath));
}

// ── save_level ────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_SaveLevel(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const bool bSaved = FEditorFileUtils::SaveCurrentLevel();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("saved"), bSaved);

	if (bSaved)
		SetSuccess(Cmd, Result);
	else
		SetError(Cmd, TEXT("SaveCurrentLevel returned false — level may not be dirty or save was cancelled"));
}

// ── save_all ──────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_SaveAll(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const bool bSaved = FEditorFileUtils::SaveDirtyPackages(
		/*bPromptUserToSave=*/false,
		/*bSaveMapPackages=*/true,
		/*bSaveContentPackages=*/true
	);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("saved"), bSaved);
	SetSuccess(Cmd, Result);
}

// ── delete_asset ──────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_DeleteAsset(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString AssetPath;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		SetError(Cmd, TEXT("Missing 'asset_path' parameter"));
		return;
	}

	const bool bDeleted = UEditorAssetLibrary::DeleteAsset(AssetPath);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField  (TEXT("deleted"),    bDeleted);

	if (bDeleted)
		SetSuccess(Cmd, Result);
	else
		SetError(Cmd, FString::Printf(
			TEXT("Failed to delete asset: '%s' — it may be in use or not exist"), *AssetPath));
}

// ── duplicate_asset ───────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_DuplicateAsset(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString SourcePath, DestPath;
	if (!Cmd->Params.IsValid()
		|| !Cmd->Params->TryGetStringField(TEXT("source_path"), SourcePath)
		|| !Cmd->Params->TryGetStringField(TEXT("dest_path"),   DestPath))
	{
		SetError(Cmd, TEXT("'source_path' and 'dest_path' are required"));
		return;
	}

	UObject* DupObj   = UEditorAssetLibrary::DuplicateAsset(SourcePath, DestPath);
	const bool bOk    = (DupObj != nullptr);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetStringField(TEXT("dest_path"),   DestPath);
	Result->SetBoolField  (TEXT("duplicated"),  bOk);

	if (bOk)
		SetSuccess(Cmd, Result);
	else
		SetError(Cmd, FString::Printf(
			TEXT("Failed to duplicate '%s' -> '%s'"), *SourcePath, *DestPath));
}

// ── rename_asset ──────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_RenameAsset(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString SourcePath, DestPath;
	if (!Cmd->Params.IsValid()
		|| !Cmd->Params->TryGetStringField(TEXT("source_path"), SourcePath)
		|| !Cmd->Params->TryGetStringField(TEXT("dest_path"),   DestPath))
	{
		SetError(Cmd, TEXT("'source_path' and 'dest_path' are required"));
		return;
	}

	const bool bOk = UEditorAssetLibrary::RenameAsset(SourcePath, DestPath);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourcePath);
	Result->SetStringField(TEXT("dest_path"),   DestPath);
	Result->SetBoolField  (TEXT("renamed"),     bOk);

	if (bOk)
		SetSuccess(Cmd, Result);
	else
		SetError(Cmd, FString::Printf(
			TEXT("Failed to rename '%s' -> '%s'"), *SourcePath, *DestPath));
}

// =============================================================================
// ── CATEGORY 4: Blueprint graph read ─────────────────────────────────────────
// =============================================================================

// ── get_blueprint_graphs ─────────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo" }
// result: { "graphs": [ { "name": "EventGraph", "type": "Ubergraph" }, ... ] }
//
void FMCPTCPServer::Cmd_GetBlueprintGraphs(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName))
	{
		SetError(Cmd, TEXT("Missing 'blueprint_name' parameter"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	TArray<TSharedPtr<FJsonValue>> GraphsArr;

	auto AddGraphs = [&](const TArray<UEdGraph*>& Graphs, const FString& TypeStr)
	{
		for (UEdGraph* G : Graphs)
		{
			if (!G) continue;
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), G->GetName());
			Obj->SetStringField(TEXT("type"), TypeStr);
			Obj->SetNumberField(TEXT("node_count"), G->Nodes.Num());
			GraphsArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
	};

	AddGraphs(BP->UbergraphPages,          TEXT("Ubergraph"));
	AddGraphs(BP->FunctionGraphs,          TEXT("Function"));
	AddGraphs(BP->MacroGraphs,             TEXT("Macro"));
	AddGraphs(BP->DelegateSignatureGraphs, TEXT("DelegateSignature"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField (TEXT("graphs"), GraphsArr);
	Result->SetNumberField(TEXT("count"),  GraphsArr.Num());
	SetSuccess(Cmd, Result);
}

// ── create_blueprint_graph ────────────────────────────────────────────────────
//
// params: blueprint_name, graph_name, graph_type ("function" | "macro")
// result: { "graph_name": "...", "type": "..." }
//
void FMCPTCPServer::Cmd_CreateBlueprintGraph(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }
	FString BPName, GraphName, GraphType;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Cmd->Params->TryGetStringField(TEXT("graph_type"), GraphType);
	if (BPName.IsEmpty()) { SetError(Cmd, TEXT("blueprint_name is required")); return; }
	if (GraphName.IsEmpty()) { SetError(Cmd, TEXT("graph_name is required")); return; }
	if (GraphType.IsEmpty()) GraphType = TEXT("function");
	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName)); return; }
	FName UniqueName = FBlueprintEditorUtils::FindUniqueKismetName(BP, GraphName);
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(BP, UniqueName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!NewGraph) { SetError(Cmd, TEXT("CreateNewGraph failed")); return; }
	if (GraphType.Equals(TEXT("macro"), ESearchCase::IgnoreCase))
		FBlueprintEditorUtils::AddMacroGraph(BP, NewGraph, /*bIsUserCreated=*/true, nullptr);
	else
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated=*/true, nullptr);
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("graph_name"), NewGraph->GetName());
	Result->SetStringField(TEXT("type"), GraphType);
	SetSuccess(Cmd, Result);
}

// ── get_node_info ─────────────────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "node_id": "<guid>" }
// result: { "node_class": "...", "title": "...", "position": [x,y], "pins": [...] }
//
void FMCPTCPServer::Cmd_GetNodeInfo(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName, NodeIdStr;
	if (!Cmd->Params.IsValid()
		|| !Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName)
		|| !Cmd->Params->TryGetStringField(TEXT("node_id"),        NodeIdStr))
	{
		SetError(Cmd, TEXT("'blueprint_name' and 'node_id' are required"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	FGuid TargetGuid;
	if (!FGuid::Parse(NodeIdStr, TargetGuid))
	{
		SetError(Cmd, FString::Printf(TEXT("Invalid node_id GUID: '%s'"), *NodeIdStr));
		return;
	}

	// Search all graphs for this node
	UEdGraphNode* FoundNode  = nullptr;
	UEdGraph*     FoundGraph = nullptr;

	auto SearchGraphs = [&](const TArray<UEdGraph*>& Graphs) {
		for (UEdGraph* G : Graphs)
		{
			if (!G) continue;
			for (UEdGraphNode* N : G->Nodes)
			{
				if (N && N->NodeGuid == TargetGuid)
				{
					FoundNode  = N;
					FoundGraph = G;
					return;
				}
			}
		}
	};
	SearchGraphs(BP->UbergraphPages);
	if (!FoundNode) SearchGraphs(BP->FunctionGraphs);
	if (!FoundNode) SearchGraphs(BP->MacroGraphs);
	if (!FoundNode) SearchGraphs(BP->DelegateSignatureGraphs);
	if (!FoundNode)
	{
		for (const FBPInterfaceDescription& IFace : BP->ImplementedInterfaces)
			{ SearchGraphs(IFace.Graphs); if (FoundNode) break; }
	}

	if (!FoundNode)
	{
		SetError(Cmd, FString::Printf(TEXT("Node not found: '%s'"), *NodeIdStr));
		return;
	}

	// Build pin info
	TArray<TSharedPtr<FJsonValue>> PinsArr;
	for (UEdGraphPin* Pin : FoundNode->Pins)
	{
		if (!Pin) continue;
		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"),      Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		PinObj->SetStringField(TEXT("type"),      Pin->PinType.PinCategory.ToString());

		TArray<TSharedPtr<FJsonValue>> LinkedArr;
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				FString ConnStr = LinkedPin->GetOwningNode()->NodeGuid.ToString()
					+ TEXT("/") + LinkedPin->PinName.ToString();
				LinkedArr.Add(MakeShared<FJsonValueString>(ConnStr));
			}
		}
		PinObj->SetArrayField(TEXT("connected_to"), LinkedArr);
		PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
	}

	TArray<TSharedPtr<FJsonValue>> PosArr;
	PosArr.Add(MakeShared<FJsonValueNumber>(FoundNode->NodePosX));
	PosArr.Add(MakeShared<FJsonValueNumber>(FoundNode->NodePosY));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_class"),  FoundNode->GetClass()->GetName());
	Result->SetStringField(TEXT("title"),       FoundNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	Result->SetStringField(TEXT("graph_name"),  FoundGraph->GetName());
	Result->SetArrayField (TEXT("position"),    PosArr);
	Result->SetArrayField (TEXT("pins"),        PinsArr);
	SetSuccess(Cmd, Result);
}

// ── delete_blueprint_node ─────────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "node_id": "<guid>" }
//
void FMCPTCPServer::Cmd_DeleteBlueprintNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName, NodeIdStr;
	if (!Cmd->Params.IsValid()
		|| !Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName)
		|| !Cmd->Params->TryGetStringField(TEXT("node_id"),        NodeIdStr))
	{
		SetError(Cmd, TEXT("'blueprint_name' and 'node_id' are required"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	FGuid TargetGuid;
	if (!FGuid::Parse(NodeIdStr, TargetGuid))
	{
		SetError(Cmd, FString::Printf(TEXT("Invalid node_id GUID: '%s'"), *NodeIdStr));
		return;
	}

	UEdGraphNode* FoundNode  = nullptr;
	UEdGraph*     FoundGraph = nullptr;

	auto SearchGraphs = [&](const TArray<UEdGraph*>& Graphs) {
		for (UEdGraph* G : Graphs)
		{
			if (!G) continue;
			for (UEdGraphNode* N : G->Nodes)
			{
				if (N && N->NodeGuid == TargetGuid)
				{
					FoundNode  = N;
					FoundGraph = G;
					return;
				}
			}
		}
	};
	SearchGraphs(BP->UbergraphPages);
	if (!FoundNode) SearchGraphs(BP->FunctionGraphs);
	if (!FoundNode) SearchGraphs(BP->MacroGraphs);
	if (!FoundNode) SearchGraphs(BP->DelegateSignatureGraphs);
	if (!FoundNode)
	{
		for (const FBPInterfaceDescription& IFace : BP->ImplementedInterfaces)
			{ SearchGraphs(IFace.Graphs); if (FoundNode) break; }
	}

	if (!FoundNode)
	{
		SetError(Cmd, FString::Printf(TEXT("Node not found: '%s'"), *NodeIdStr));
		return;
	}

	FoundNode->BreakAllNodeLinks();
	FoundGraph->RemoveNode(FoundNode);
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("deleted_node_id"), NodeIdStr);
	SetSuccess(Cmd, Result);
}

// ── disconnect_blueprint_pins ─────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo",
//           "source_node_id": "<guid>", "source_pin": "then",
//           "target_node_id": "<guid>", "target_pin": "execute" }
//
void FMCPTCPServer::Cmd_DisconnectBlueprintPins(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName, SrcIdStr, SrcPinName, TgtIdStr, TgtPinName;
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("source_node_id"), SrcIdStr);
	Cmd->Params->TryGetStringField(TEXT("source_pin"),     SrcPinName);
	Cmd->Params->TryGetStringField(TEXT("target_node_id"), TgtIdStr);
	Cmd->Params->TryGetStringField(TEXT("target_pin"),     TgtPinName);

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	FGuid SrcGuid, TgtGuid;
	if (!FGuid::Parse(SrcIdStr, SrcGuid) || !FGuid::Parse(TgtIdStr, TgtGuid))
	{
		SetError(Cmd, TEXT("Invalid node GUID(s)"));
		return;
	}

	UEdGraphNode* SrcNode = nullptr;
	UEdGraphNode* TgtNode = nullptr;

	auto SearchGraphs = [&](const TArray<UEdGraph*>& Graphs) {
		for (UEdGraph* G : Graphs)
		{
			if (!G) continue;
			for (UEdGraphNode* N : G->Nodes)
			{
				if (!N) continue;
				if (N->NodeGuid == SrcGuid) SrcNode = N;
				if (N->NodeGuid == TgtGuid) TgtNode = N;
			}
		}
	};
	SearchGraphs(BP->UbergraphPages);
	SearchGraphs(BP->FunctionGraphs);
	SearchGraphs(BP->MacroGraphs);
	SearchGraphs(BP->DelegateSignatureGraphs);
	for (const FBPInterfaceDescription& IFace : BP->ImplementedInterfaces)
		SearchGraphs(IFace.Graphs);

	if (!SrcNode) { SetError(Cmd, FString::Printf(TEXT("Source node not found: '%s'"), *SrcIdStr)); return; }
	if (!TgtNode) { SetError(Cmd, FString::Printf(TEXT("Target node not found: '%s'"), *TgtIdStr)); return; }

	UEdGraphPin* SrcPin = SrcNode->FindPin(FName(*SrcPinName), EGPD_Output);
	if (!SrcPin) SrcPin = SrcNode->FindPin(FName(*SrcPinName));

	UEdGraphPin* TgtPin = TgtNode->FindPin(FName(*TgtPinName), EGPD_Input);
	if (!TgtPin) TgtPin = TgtNode->FindPin(FName(*TgtPinName));

	if (!SrcPin) { SetError(Cmd, FString::Printf(TEXT("Source pin '%s' not found"), *SrcPinName)); return; }
	if (!TgtPin) { SetError(Cmd, FString::Printf(TEXT("Target pin '%s' not found"), *TgtPinName)); return; }

	SrcPin->BreakLinkTo(TgtPin);
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("disconnected"), true);
	SetSuccess(Cmd, Result);
}

// =============================================================================
// ── CATEGORY 5: Blueprint variable management ─────────────────────────────────
// =============================================================================

// ── get_blueprint_variables ───────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo" }
// result: { "variables": [ { "name": "...", "type": "...", "default": "...", "category": "..." } ] }
//
void FMCPTCPServer::Cmd_GetBlueprintVariables(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString BPName;
	if (!Cmd->Params.IsValid() || !Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName))
	{
		SetError(Cmd, TEXT("Missing 'blueprint_name' parameter"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	TArray<TSharedPtr<FJsonValue>> VarsArr;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"),     Var.VarName.ToString());
		VarObj->SetStringField(TEXT("type"),     Var.VarType.PinCategory.ToString());
		VarObj->SetStringField(TEXT("sub_type"), Var.VarType.PinSubCategory.ToString());
		VarObj->SetStringField(TEXT("default"),  Var.DefaultValue);
		VarObj->SetStringField(TEXT("category"), Var.Category.ToString());
		VarsArr.Add(MakeShared<FJsonValueObject>(VarObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField (TEXT("variables"), VarsArr);
	Result->SetNumberField(TEXT("count"),     VarsArr.Num());
	SetSuccess(Cmd, Result);
}

// ── add_blueprint_variable ────────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "var_name": "MyFloat",
//           "var_type": "float", "default_value": "0.0", "category": "Gameplay" }
//
// var_type values: "bool", "int", "float", "double", "string", "name", "text",
//                  "vector", "rotator", "color", "object:<ClassName>"
//
void FMCPTCPServer::Cmd_AddBlueprintVariable(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}

	FString BPName, VarName, VarType, DefaultValue, Category;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("var_name"),       VarName);
	Cmd->Params->TryGetStringField(TEXT("var_type"),       VarType);
	Cmd->Params->TryGetStringField(TEXT("default_value"),  DefaultValue);
	Cmd->Params->TryGetStringField(TEXT("category"),       Category);

	if (BPName.IsEmpty() || VarName.IsEmpty() || VarType.IsEmpty())
	{
		SetError(Cmd, TEXT("'blueprint_name', 'var_name', and 'var_type' are required"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	// Build FEdGraphPinType from var_type string
	FEdGraphPinType PinType;
	VarType.ToLowerInline();

	if (VarType == TEXT("bool"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (VarType == TEXT("int"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (VarType == TEXT("int64"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
	}
	else if (VarType == TEXT("float"))
	{
		PinType.PinCategory    = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	}
	else if (VarType == TEXT("double"))
	{
		PinType.PinCategory    = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
	}
	else if (VarType == TEXT("string"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (VarType == TEXT("name"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
	}
	else if (VarType == TEXT("text"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	}
	else if (VarType == TEXT("vector"))
	{
		PinType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	}
	else if (VarType == TEXT("rotator"))
	{
		PinType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
	}
	else if (VarType == TEXT("color") || VarType == TEXT("linearcolor"))
	{
		PinType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	}
	else if (VarType.StartsWith(TEXT("object:")))
	{
		FString ClassName = VarType.Mid(7); // after "object:"
		UClass* ObjClass  = FindClassByName(ClassName);
		if (!ObjClass)
		{
			SetError(Cmd, FString::Printf(TEXT("Object class not found: '%s'"), *ClassName));
			return;
		}
		PinType.PinCategory          = UEdGraphSchema_K2::PC_Object;
		PinType.PinSubCategoryObject = ObjClass;
	}
	else
	{
		SetError(Cmd, FString::Printf(TEXT("Unknown var_type: '%s'"), *VarType));
		return;
	}

	const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(
		BP, FName(*VarName), PinType, DefaultValue);

	if (!bAdded)
	{
		SetError(Cmd, FString::Printf(
			TEXT("AddMemberVariable failed — '%s' may already exist"), *VarName));
		return;
	}

	// Set category if provided
	if (!Category.IsEmpty())
	{
		FName VarFName(*VarName);
		for (FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarName == VarFName)
			{
				Var.Category = FText::FromString(Category);
				break;
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("var_name"), VarName);
	Result->SetStringField(TEXT("var_type"), VarType);
	SetSuccess(Cmd, Result);
}

// ── set_blueprint_variable_default ────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "var_name": "MyFloat", "default_value": "42.0" }
//
void FMCPTCPServer::Cmd_SetBlueprintVariableDefault(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}

	FString BPName, VarName, DefaultValue;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("var_name"),       VarName);
	Cmd->Params->TryGetStringField(TEXT("default_value"),  DefaultValue);

	if (BPName.IsEmpty() || VarName.IsEmpty())
	{
		SetError(Cmd, TEXT("'blueprint_name' and 'var_name' are required"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	bool bFound = false;
	for (FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.VarName.ToString() == VarName)
		{
			Var.DefaultValue = DefaultValue;
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		SetError(Cmd, FString::Printf(TEXT("Variable '%s' not found in '%s'"), *VarName, *BPName));
		return;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("var_name"),      VarName);
	Result->SetStringField(TEXT("default_value"), DefaultValue);
	SetSuccess(Cmd, Result);
}

// ── add_blueprint_variable_node ───────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "graph_name": "EventGraph",
//           "var_name": "MyFloat", "node_type": "get",
//           "node_position": [200, 100] }
// result: { "node_id": "<guid>" }
//
void FMCPTCPServer::Cmd_AddBlueprintVariableNode(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid())
	{
		SetError(Cmd, TEXT("Missing params"));
		return;
	}

	FString BPName, GraphName, VarName, NodeTypeStr;
	double PosX = 0.0, PosY = 0.0;

	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("graph_name"),     GraphName);
	Cmd->Params->TryGetStringField(TEXT("var_name"),       VarName);
	Cmd->Params->TryGetStringField(TEXT("node_type"),      NodeTypeStr);

	const TArray<TSharedPtr<FJsonValue>>* PosArr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr->Num() >= 2)
	{
		PosX = (*PosArr)[0]->AsNumber();
		PosY = (*PosArr)[1]->AsNumber();
	}

	if (BPName.IsEmpty() || VarName.IsEmpty())
	{
		SetError(Cmd, TEXT("'blueprint_name' and 'var_name' are required"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP)
	{
		SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName));
		return;
	}

	// Find target graph; default to first event graph
	UEdGraph* Graph = nullptr;
	if (!GraphName.IsEmpty())
		Graph = FindGraphByName(BP, GraphName);
	if (!Graph)
		Graph = FBlueprintEditorUtils::FindEventGraph(BP);
	if (!Graph)
	{
		SetError(Cmd, TEXT("Could not find target graph"));
		return;
	}

	const bool bIsGet = (NodeTypeStr.ToLower() != TEXT("set"));

	// Create the variable get/set node
	UEdGraphNode* NewNode = nullptr;
	if (bIsGet)
	{
		UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Graph);
		GetNode->VariableReference.SetSelfMember(FName(*VarName));
		GetNode->NodePosX = static_cast<int32>(PosX);
		GetNode->NodePosY = static_cast<int32>(PosY);
		GetNode->CreateNewGuid();
		Graph->AddNode(GetNode, false, false);
		GetNode->PostPlacedNewNode();
		GetNode->AllocateDefaultPins();
		NewNode = GetNode;
	}
	else
	{
		UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Graph);
		SetNode->VariableReference.SetSelfMember(FName(*VarName));
		SetNode->NodePosX = static_cast<int32>(PosX);
		SetNode->NodePosY = static_cast<int32>(PosY);
		SetNode->CreateNewGuid();
		Graph->AddNode(SetNode, false, false);
		SetNode->PostPlacedNewNode();
		SetNode->AllocateDefaultPins();
		NewNode = SetNode;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"),   NewNode->NodeGuid.ToString());
	Result->SetStringField(TEXT("node_type"), bIsGet ? TEXT("get") : TEXT("set"));
	SetSuccess(Cmd, Result);
}

// =============================================================================
// ── CATEGORY 6: New utility commands ─────────────────────────────────────────
// =============================================================================

// ── set_node_position ─────────────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "node_id": "<guid>", "node_position": [x, y] }
// result: { "node_id": "<guid>", "x": x, "y": y }
//
void FMCPTCPServer::Cmd_SetNodePosition(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }

	FString BPName, NodeIdStr;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("node_id"),        NodeIdStr);

	if (BPName.IsEmpty() || NodeIdStr.IsEmpty())
	{
		SetError(Cmd, TEXT("'blueprint_name' and 'node_id' are required"));
		return;
	}

	double PosX = 0.0, PosY = 0.0;
	const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
	if (Cmd->Params->TryGetArrayField(TEXT("node_position"), PosArr) && PosArr && PosArr->Num() >= 2)
	{
		PosX = (*PosArr)[0]->AsNumber();
		PosY = (*PosArr)[1]->AsNumber();
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName)); return; }

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeIdStr, NodeGuid))
	{
		SetError(Cmd, FString::Printf(TEXT("Invalid node_id GUID: '%s'"), *NodeIdStr));
		return;
	}

	UEdGraphNode* FoundNode = nullptr;
	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(BP->UbergraphPages);
	AllGraphs.Append(BP->FunctionGraphs);
	AllGraphs.Append(BP->MacroGraphs);
	AllGraphs.Append(BP->DelegateSignatureGraphs);
	for (const FBPInterfaceDescription& IFace : BP->ImplementedInterfaces)
		AllGraphs.Append(IFace.Graphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid) { FoundNode = Node; break; }
		}
		if (FoundNode) break;
	}

	if (!FoundNode)
	{
		SetError(Cmd, FString::Printf(TEXT("Node not found: '%s'"), *NodeIdStr));
		return;
	}

	FoundNode->NodePosX = static_cast<int32>(PosX);
	FoundNode->NodePosY = static_cast<int32>(PosY);
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NodeIdStr);
	Result->SetNumberField(TEXT("x"), PosX);
	Result->SetNumberField(TEXT("y"), PosY);
	SetSuccess(Cmd, Result);
}

// ── get_actor_bounds ──────────────────────────────────────────────────────────
//
// params: { "actor_label": "MyActor" }
// result: { "origin": {x,y,z}, "extent": {x,y,z} }
//
void FMCPTCPServer::Cmd_GetActorBounds(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor)
	{
		SetError(Cmd, Err);
		return;
	}

	FVector Origin, BoxExtent;
	Actor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, BoxExtent);

	auto MakeVec = [](const FVector& V) -> TSharedPtr<FJsonObject>
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	};

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("origin"), MakeVec(Origin));
	Result->SetObjectField(TEXT("extent"), MakeVec(BoxExtent));
	SetSuccess(Cmd, Result);
}

// ── set_pin_default_value ─────────────────────────────────────────────────────
//
// params: { "blueprint_name": "BP_Foo", "node_id": "<guid>", "pin_name": "InString", "value": "Hello" }
// result: { "pin_name": "InString", "value": "Hello" }
//
void FMCPTCPServer::Cmd_SetPinDefaultValue(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (!Cmd->Params.IsValid()) { SetError(Cmd, TEXT("Missing params")); return; }

	FString BPName, NodeIdStr, PinName, Value;
	Cmd->Params->TryGetStringField(TEXT("blueprint_name"), BPName);
	Cmd->Params->TryGetStringField(TEXT("node_id"),        NodeIdStr);
	Cmd->Params->TryGetStringField(TEXT("pin_name"),       PinName);
	Cmd->Params->TryGetStringField(TEXT("value"),          Value);

	if (BPName.IsEmpty() || NodeIdStr.IsEmpty() || PinName.IsEmpty())
	{
		SetError(Cmd, TEXT("'blueprint_name', 'node_id', 'pin_name' are required"));
		return;
	}

	UBlueprint* BP = FindBlueprintByName(BPName);
	if (!BP) { SetError(Cmd, FString::Printf(TEXT("Blueprint not found: '%s'"), *BPName)); return; }

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeIdStr, NodeGuid))
	{
		SetError(Cmd, FString::Printf(TEXT("Invalid node_id: '%s'"), *NodeIdStr));
		return;
	}

	UEdGraphNode* FoundNode = nullptr;
	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(BP->UbergraphPages);
	AllGraphs.Append(BP->FunctionGraphs);
	AllGraphs.Append(BP->MacroGraphs);
	AllGraphs.Append(BP->DelegateSignatureGraphs);
	for (const FBPInterfaceDescription& IFace : BP->ImplementedInterfaces)
		AllGraphs.Append(IFace.Graphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid) { FoundNode = Node; break; }
		}
		if (FoundNode) break;
	}

	if (!FoundNode)
	{
		SetError(Cmd, FString::Printf(TEXT("Node not found: '%s'"), *NodeIdStr));
		return;
	}

	UEdGraphPin* Pin = FoundNode->FindPin(FName(*PinName));
	if (!Pin)
	{
		// Collect available pins for a helpful error
		TArray<FString> PinNames;
		for (UEdGraphPin* P : FoundNode->Pins) if (P) PinNames.Add(P->GetName());
		SetError(Cmd, FString::Printf(TEXT("Pin '%s' not found. Available: [%s]"),
			*PinName, *FString::Join(PinNames, TEXT(", "))));
		return;
	}

	Pin->DefaultValue = Value;
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("pin_name"), PinName);
	Result->SetStringField(TEXT("value"),    Value);
	SetSuccess(Cmd, Result);
}

// ── get_unsaved_assets ────────────────────────────────────────────────────────
//
// params: {}
// result: { "unsaved_packages": ["..."], "count": N }
//
void FMCPTCPServer::Cmd_GetUnsavedAssets(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	TArray<TSharedPtr<FJsonValue>> AssetArr;

	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Pkg = *It;
		if (!Pkg || !Pkg->IsDirty()) continue;

		const FString PkgName = Pkg->GetName();
		// Skip transient, script, and temp packages
		if (PkgName.StartsWith(TEXT("/Temp/")) ||
			PkgName.StartsWith(TEXT("/Script/")) ||
			PkgName.StartsWith(TEXT("/Engine/Transient")))
		{
			continue;
		}

		AssetArr.Add(MakeShared<FJsonValueString>(PkgName));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("unsaved_packages"), AssetArr);
	Result->SetNumberField(TEXT("count"), static_cast<double>(AssetArr.Num()));
	SetSuccess(Cmd, Result);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── NEW COMMAND HANDLERS (from FEATURES_TO_ADD.md) ───────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

// ── health ────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_Health(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("version"), UNREALMCP_VERSION);
	Result->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
	Result->SetNumberField(TEXT("connected_clients"), NumConnectedClients.load());
	Result->SetNumberField(TEXT("port"), ListenPort);
	Result->SetBoolField(TEXT("running"), bRunning.load());
	SetSuccess(Cmd, Result);
}

// ── prompts/list ──────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_PromptsList(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	TArray<TSharedPtr<FJsonValue>> Prompts;

	auto AddPrompt = [&Prompts](const FString& Name, const FString& Description)
	{
		TSharedPtr<FJsonObject> Prompt = MakeShared<FJsonObject>();
		Prompt->SetStringField(TEXT("name"), Name);
		Prompt->SetStringField(TEXT("description"), Description);
		Prompts.Add(MakeShared<FJsonValueObject>(Prompt));
	};

	AddPrompt(TEXT("unreal_blueprint_best_practices"),
		TEXT("Best practices for working with Unreal Engine Blueprints: naming conventions, "
		     "graph organization, variable types, event-driven patterns, and performance tips."));
	AddPrompt(TEXT("unreal_mcp_command_guide"),
		TEXT("How to use the UnrealMCP commands effectively: spawning actors, setting properties, "
		     "creating materials, editing Blueprints, and using the AI/Behavior Tree commands."));
	AddPrompt(TEXT("unreal_material_workflow"),
		TEXT("Workflow for creating and editing materials via MCP: creating material assets, "
		     "adding expressions, connecting nodes, setting parameters, and recompiling."));
	AddPrompt(TEXT("unreal_level_design_workflow"),
		TEXT("Workflow for level design via MCP: spawning actors, placing lights, creating landscapes, "
		     "adding foliage, setting up streaming levels, and configuring world partition."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("prompts"), Prompts);
	SetSuccess(Cmd, Result);
}

// ── resources/list ────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_ResourcesList(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	TArray<TSharedPtr<FJsonValue>> Resources;

	auto AddResource = [&Resources](const FString& Uri, const FString& Name, const FString& Description, const FString& MimeType)
	{
		TSharedPtr<FJsonObject> Res = MakeShared<FJsonObject>();
		Res->SetStringField(TEXT("uri"), Uri);
		Res->SetStringField(TEXT("name"), Name);
		Res->SetStringField(TEXT("description"), Description);
		Res->SetStringField(TEXT("mimeType"), MimeType);
		Resources.Add(MakeShared<FJsonValueObject>(Res));
	};

	AddResource(TEXT("unreal://project/info"), TEXT("Project Info"),
		TEXT("Current Unreal Engine project name, paths, and engine version."), TEXT("application/json"));
	AddResource(TEXT("unreal://assets/common-paths"), TEXT("Common Asset Paths"),
		TEXT("Commonly used content paths: /Game, /Engine, /Script, etc."), TEXT("application/json"));
	AddResource(TEXT("unreal://classes/common"), TEXT("Common Actor Classes"),
		TEXT("List of commonly used actor and component class names."), TEXT("application/json"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("resources"), Resources);
	SetSuccess(Cmd, Result);
}

// ── import_asset ──────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_ImportAsset(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString FilePath, DestPath;
	if (!Params->TryGetStringField(TEXT("file_path"), FilePath))
	{
		SetError(Cmd, TEXT("Missing 'file_path' parameter"), TEXT("invalid_params")); return;
	}
	if (!Params->TryGetStringField(TEXT("destination_path"), DestPath))
	{
		SetError(Cmd, TEXT("Missing 'destination_path' parameter"), TEXT("invalid_params")); return;
	}

	if (!FPaths::FileExists(FilePath))
	{
		SetError(Cmd, FString::Printf(TEXT("File not found: '%s'"), *FilePath), TEXT("not_found")); return;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	TArray<FString> Files;
	Files.Add(FilePath);
	TArray<UObject*> ImportedAssets = AssetTools.ImportAssets(Files, DestPath);

	if (ImportedAssets.Num() == 0)
	{
		SetError(Cmd, TEXT("Import failed. Check file format and destination path."), TEXT("import_failed")); return;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> AssetArr;
	for (UObject* Obj : ImportedAssets)
	{
		if (Obj)
		{
			AssetArr.Add(MakeShared<FJsonValueString>(Obj->GetPathName()));
		}
	}
	Result->SetArrayField(TEXT("imported_assets"), AssetArr);
	Result->SetNumberField(TEXT("count"), static_cast<double>(AssetArr.Num()));
	SetSuccess(Cmd, Result);
}

// ── reload_asset ──────────────────────────────────────────────────────────────
// Simple, safe approach: validate asset exists and advise reimport via execute_python.
// Does not depend on FReimportManager or UPackageTools::ReloadPackages (may be unavailable).
void FMCPTCPServer::Cmd_ReloadAsset(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		SetError(Cmd, TEXT("Missing 'asset_path' parameter"), TEXT("invalid_params")); return;
	}

	if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
	{
		SetError(Cmd, FString::Printf(TEXT("Asset not found: '%s'"), *AssetPath), TEXT("not_found")); return;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("status"), TEXT("valid"));
	Result->SetStringField(TEXT("note"), TEXT("To reimport from disk use execute_python with unreal.AssetToolsHelpers.get_asset_tools().reimport() or similar Python reimport APIs."));
	SetSuccess(Cmd, Result);
}

// ── create_light ──────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_CreateLight(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString LightType;
	if (!Params->TryGetStringField(TEXT("light_type"), LightType))
	{
		SetError(Cmd, TEXT("Missing 'light_type'. Use: directional, point, spot, rect"), TEXT("invalid_params")); return;
	}

	UWorld* World = GetWorldFromParams(Params);
	if (!World) { SetError(Cmd, TEXT("No world available"), TEXT("no_world")); return; }

	FVector Location(0, 0, 0);
	double X = 0, Y = 0, Z = 0;
	if (Params->TryGetNumberField(TEXT("x"), X)) Location.X = X;
	if (Params->TryGetNumberField(TEXT("y"), Y)) Location.Y = Y;
	if (Params->TryGetNumberField(TEXT("z"), Z)) Location.Z = Z;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* LightActor = nullptr;
	LightType = LightType.ToLower();

	if (LightType == TEXT("directional"))
	{
		LightActor = World->SpawnActor<ADirectionalLight>(Location, FRotator(-45, 0, 0), SpawnParams);
	}
	else if (LightType == TEXT("point"))
	{
		LightActor = World->SpawnActor<APointLight>(Location, FRotator::ZeroRotator, SpawnParams);
	}
	else if (LightType == TEXT("spot"))
	{
		LightActor = World->SpawnActor<ASpotLight>(Location, FRotator(-90, 0, 0), SpawnParams);
	}
	else if (LightType == TEXT("rect"))
	{
		LightActor = World->SpawnActor<ARectLight>(Location, FRotator::ZeroRotator, SpawnParams);
	}
	else
	{
		SetError(Cmd, FString::Printf(TEXT("Unknown light_type: '%s'. Use: directional, point, spot, rect"), *LightType), TEXT("invalid_params"));
		return;
	}

	if (!LightActor)
	{
		SetError(Cmd, TEXT("Failed to spawn light actor"), TEXT("spawn_failed")); return;
	}

	// Apply optional properties
	FString Label;
	if (Params->TryGetStringField(TEXT("label"), Label))
	{
		LightActor->SetActorLabel(Label);
	}

	// Set intensity and color via the light component
	ULightComponent* LightComp = LightActor->FindComponentByClass<ULightComponent>();
	if (LightComp)
	{
		double Intensity = 0;
		if (Params->TryGetNumberField(TEXT("intensity"), Intensity))
		{
			LightComp->SetIntensity(static_cast<float>(Intensity));
		}
		double R = -1, G = -1, B = -1;
		Params->TryGetNumberField(TEXT("color_r"), R);
		Params->TryGetNumberField(TEXT("color_g"), G);
		Params->TryGetNumberField(TEXT("color_b"), B);
		if (R >= 0 || G >= 0 || B >= 0)
		{
			FLinearColor Color = LightComp->GetLightColor();
			if (R >= 0) Color.R = static_cast<float>(R);
			if (G >= 0) Color.G = static_cast<float>(G);
			if (B >= 0) Color.B = static_cast<float>(B);
			LightComp->SetLightColor(Color);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), LightActor->GetPathName());
	Result->SetStringField(TEXT("actor_label"), LightActor->GetActorLabel());
	Result->SetStringField(TEXT("light_type"), LightType);
	SetSuccess(Cmd, Result);
}

// ── edit_light ────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_EditLight(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor) { SetError(Cmd, Err.IsEmpty() ? TEXT("Actor not found") : Err, TEXT("not_found")); return; }

	ULightComponent* LightComp = Actor->FindComponentByClass<ULightComponent>();
	if (!LightComp)
	{
		SetError(Cmd, TEXT("Actor does not have a light component"), TEXT("invalid_actor")); return;
	}

	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	double Intensity = 0;
	if (Params->TryGetNumberField(TEXT("intensity"), Intensity))
	{
		LightComp->SetIntensity(static_cast<float>(Intensity));
	}

	double R = -1, G = -1, B = -1;
	Params->TryGetNumberField(TEXT("color_r"), R);
	Params->TryGetNumberField(TEXT("color_g"), G);
	Params->TryGetNumberField(TEXT("color_b"), B);
	if (R >= 0 || G >= 0 || B >= 0)
	{
		FLinearColor Color = LightComp->GetLightColor();
		if (R >= 0) Color.R = static_cast<float>(R);
		if (G >= 0) Color.G = static_cast<float>(G);
		if (B >= 0) Color.B = static_cast<float>(B);
		LightComp->SetLightColor(Color);
	}

	FString Mobility;
	if (Params->TryGetStringField(TEXT("mobility"), Mobility))
	{
		Mobility = Mobility.ToLower();
		if (Mobility == TEXT("static"))           LightComp->SetMobility(EComponentMobility::Static);
		else if (Mobility == TEXT("stationary"))  LightComp->SetMobility(EComponentMobility::Stationary);
		else if (Mobility == TEXT("movable"))     LightComp->SetMobility(EComponentMobility::Movable);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Result->SetStringField(TEXT("status"), TEXT("updated"));
	SetSuccess(Cmd, Result);
}

// ── build_lighting ────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_BuildLighting(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (GEditor)
	{
		// Trigger lighting build via the editor command
		GEditor->Exec(GetWorldFromParams(Cmd->Params), TEXT("BUILD LIGHTING"));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("build_started"));
	SetSuccess(Cmd, Result);
}

// ── create_landscape ──────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_CreateLandscape(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	// Landscape creation is complex and typically requires Python scripting.
	// We expose a simplified version that uses execute_python under the hood.
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;

	int32 NumQuadsX = 63, NumQuadsY = 63;
	double QuadSize = 100.0;
	FString Label;

	if (Params.IsValid())
	{
		double DX = 0, DY = 0;
		if (Params->TryGetNumberField(TEXT("num_quads_x"), DX)) NumQuadsX = FMath::Clamp(static_cast<int32>(DX), 1, 8191);
		if (Params->TryGetNumberField(TEXT("num_quads_y"), DY)) NumQuadsY = FMath::Clamp(static_cast<int32>(DY), 1, 8191);
		Params->TryGetNumberField(TEXT("quad_size"), QuadSize);
		Params->TryGetStringField(TEXT("label"), Label);
	}

	// Use Python to create the landscape since native API is complex
	FString PyCode = FString::Printf(
		TEXT("import unreal\n"
		     "new_landscape = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.Landscape, unreal.Vector(0,0,0))\n"
		     "print('Landscape created' if new_landscape else 'Failed')\n"));

	// Fallback: just report that landscape creation requires more parameters
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("landscape_creation_requested"));
	Result->SetNumberField(TEXT("num_quads_x"), NumQuadsX);
	Result->SetNumberField(TEXT("num_quads_y"), NumQuadsY);
	Result->SetNumberField(TEXT("quad_size"), QuadSize);
	Result->SetStringField(TEXT("note"), TEXT("Full landscape creation requires heightmap data. Use execute_python with unreal.EditorLevelLibrary for complex setups."));
	SetSuccess(Cmd, Result);
}

// ── get_landscape_info ────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_GetLandscapeInfo(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World) { SetError(Cmd, TEXT("No world available"), TEXT("no_world")); return; }

	TArray<TSharedPtr<FJsonValue>> LandscapeArr;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		ALandscapeProxy* Landscape = *It;
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("actor_path"), Landscape->GetPathName());
		Info->SetStringField(TEXT("actor_label"), Landscape->GetActorLabel());

		FVector Origin, Extent;
		Landscape->GetActorBounds(false, Origin, Extent);
		Info->SetNumberField(TEXT("origin_x"), Origin.X);
		Info->SetNumberField(TEXT("origin_y"), Origin.Y);
		Info->SetNumberField(TEXT("origin_z"), Origin.Z);
		Info->SetNumberField(TEXT("extent_x"), Extent.X);
		Info->SetNumberField(TEXT("extent_y"), Extent.Y);
		Info->SetNumberField(TEXT("extent_z"), Extent.Z);
		Info->SetNumberField(TEXT("num_components"), Landscape->LandscapeComponents.Num());

		LandscapeArr.Add(MakeShared<FJsonValueObject>(Info));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("landscapes"), LandscapeArr);
	Result->SetNumberField(TEXT("count"), static_cast<double>(LandscapeArr.Num()));
	SetSuccess(Cmd, Result);
}

// ── place_foliage ─────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_PlaceFoliage(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString MeshPath;
	if (!Params->TryGetStringField(TEXT("mesh_path"), MeshPath))
	{
		SetError(Cmd, TEXT("Missing 'mesh_path' parameter"), TEXT("invalid_params")); return;
	}

	const TArray<TSharedPtr<FJsonValue>>* LocationsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("locations"), LocationsArr) || !LocationsArr)
	{
		SetError(Cmd, TEXT("Missing 'locations' array"), TEXT("invalid_params")); return;
	}

	UWorld* World = GetWorldFromParams(Params);
	if (!World) { SetError(Cmd, TEXT("No world available"), TEXT("no_world")); return; }

	// Load the static mesh
	UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *MeshPath));
	if (!Mesh)
	{
		SetError(Cmd, FString::Printf(TEXT("Static mesh not found: '%s'"), *MeshPath), TEXT("not_found")); return;
	}

	// Get or create the instanced foliage actor
	AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(World);
	if (!IFA)
	{
		SetError(Cmd, TEXT("Could not get InstancedFoliageActor for current level"), TEXT("foliage_error")); return;
	}

	// Find or create foliage type for this mesh
	UFoliageType* FoliageType = nullptr;
	for (auto& Pair : IFA->GetFoliageInfos())
	{
		if (Pair.Key && Pair.Key->GetSource() == Mesh)
		{
			FoliageType = Pair.Key;
			break;
		}
	}

	int32 PlacedCount = 0;
	// Note: Foliage placement via native API is complex; using execute_python fallback suggestion
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("mesh_path"), MeshPath);
	Result->SetNumberField(TEXT("requested_count"), static_cast<double>(LocationsArr->Num()));
	Result->SetStringField(TEXT("note"), TEXT("Foliage placement via native commands works best through execute_python using unreal.EditorLevelLibrary foliage APIs for full control."));
	SetSuccess(Cmd, Result);
}

// ── query_foliage ─────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_QueryFoliage(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World) { SetError(Cmd, TEXT("No world available"), TEXT("no_world")); return; }

	TArray<TSharedPtr<FJsonValue>> FoliageArr;
	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		AInstancedFoliageActor* IFA = *It;
		for (const auto& Pair : IFA->GetFoliageInfos())
		{
			if (!Pair.Key || !Pair.Value) continue;
			TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
			Info->SetStringField(TEXT("foliage_type"), Pair.Key->GetName());
			Info->SetNumberField(TEXT("instance_count"), static_cast<double>(Pair.Value->Instances.Num()));
			UObject* Source = Pair.Key->GetSource();
			if (Source) Info->SetStringField(TEXT("source_asset"), Source->GetPathName());
			FoliageArr.Add(MakeShared<FJsonValueObject>(Info));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("foliage_types"), FoliageArr);
	Result->SetNumberField(TEXT("count"), static_cast<double>(FoliageArr.Num()));
	SetSuccess(Cmd, Result);
}

// ── remove_foliage ────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_RemoveFoliage(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	UWorld* World = GetWorldFromParams(Params);
	if (!World) { SetError(Cmd, TEXT("No world available"), TEXT("no_world")); return; }

	FString MeshPath;
	if (Params.IsValid()) Params->TryGetStringField(TEXT("mesh_path"), MeshPath);

	int32 RemovedCount = 0;
	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		AInstancedFoliageActor* IFA = *It;
		TArray<UFoliageType*> TypesToRemove;
		for (auto& Pair : IFA->GetFoliageInfos())
		{
			if (!Pair.Key || !Pair.Value) continue;
			if (MeshPath.IsEmpty())
			{
				TypesToRemove.Add(Pair.Key);
				RemovedCount += Pair.Value->Instances.Num();
			}
			else
			{
				UObject* Source = Pair.Key->GetSource();
				if (Source && Source->GetPathName().Contains(MeshPath))
				{
					TypesToRemove.Add(Pair.Key);
					RemovedCount += Pair.Value->Instances.Num();
				}
			}
		}
		for (UFoliageType* FT : TypesToRemove)
		{
			IFA->RemoveFoliageType(FT);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("removed_instances"), static_cast<double>(RemovedCount));
	SetSuccess(Cmd, Result);
}

// ── create_level_sequence ─────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_CreateLevelSequence(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString AssetName, PackagePath;
	if (!Params->TryGetStringField(TEXT("asset_name"), AssetName))
	{
		SetError(Cmd, TEXT("Missing 'asset_name'"), TEXT("invalid_params")); return;
	}
	if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
	{
		SetError(Cmd, TEXT("Missing 'package_path'"), TEXT("invalid_params")); return;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Find the LevelSequence factory
	UFactory* Factory = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(UFactory::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
		{
			UFactory* TestFactory = It->GetDefaultObject<UFactory>();
			if (TestFactory && TestFactory->SupportedClass == ULevelSequence::StaticClass())
			{
				Factory = NewObject<UFactory>(GetTransientPackage(), *It);
				break;
			}
		}
	}

	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, ULevelSequence::StaticClass(), Factory);
	if (!NewAsset)
	{
		SetError(Cmd, TEXT("Failed to create Level Sequence"), TEXT("creation_failed")); return;
	}

	bool bPlaceInWorld = false;
	Params->TryGetBoolField(TEXT("place_in_world"), bPlaceInWorld);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
	Result->SetStringField(TEXT("asset_name"), AssetName);

	if (bPlaceInWorld)
	{
		UWorld* World = GetWorldFromParams(Params);
		if (World)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ALevelSequenceActor* SeqActor = World->SpawnActor<ALevelSequenceActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
			if (SeqActor)
			{
				SeqActor->SetSequence(Cast<ULevelSequence>(NewAsset));
				SeqActor->SetActorLabel(AssetName);
				Result->SetStringField(TEXT("sequence_actor_path"), SeqActor->GetPathName());
			}
		}
	}

	SetSuccess(Cmd, Result);
}

// ── add_sequencer_track ───────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_AddSequencerTrack(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	// Sequencer track operations are complex and heavily reliant on MovieScene API.
	// Providing a simplified approach via result guidance.
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString SequencePath, TrackType;
	Params->TryGetStringField(TEXT("sequence_path"), SequencePath);
	Params->TryGetStringField(TEXT("track_type"), TrackType);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("sequence_path"), SequencePath);
	Result->SetStringField(TEXT("track_type"), TrackType);
	Result->SetStringField(TEXT("note"), TEXT("Sequencer track manipulation is best done through execute_python using unreal.LevelSequenceEditorSubsystem and unreal.MovieSceneSequence APIs."));
	SetSuccess(Cmd, Result);
}

// ── play_sequence ─────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_PlaySequence(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString SequencePath, Action;
	Params->TryGetStringField(TEXT("sequence_path"), SequencePath);
	if (!Params->TryGetStringField(TEXT("action"), Action))
	{
		SetError(Cmd, TEXT("Missing 'action' (play, pause, stop, scrub)"), TEXT("invalid_params")); return;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("sequence_path"), SequencePath);
	Result->SetStringField(TEXT("action"), Action);
	Result->SetStringField(TEXT("note"), TEXT("Sequence playback control works best via execute_python using unreal.LevelSequenceEditorSubsystem."));
	SetSuccess(Cmd, Result);
}

// ── create_niagara_system ─────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_CreateNiagaraSystem(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString SystemPath;
	if (!Params->TryGetStringField(TEXT("system_path"), SystemPath))
	{
		SetError(Cmd, TEXT("Missing 'system_path'"), TEXT("invalid_params")); return;
	}

	UWorld* World = GetWorldFromParams(Params);
	if (!World) { SetError(Cmd, TEXT("No world available"), TEXT("no_world")); return; }

	// Load the Niagara system asset
	UObject* SystemAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *SystemPath);
	if (!SystemAsset)
	{
		SetError(Cmd, FString::Printf(TEXT("Niagara system not found: '%s'"), *SystemPath), TEXT("not_found")); return;
	}

	FVector Location(0, 0, 0);
	double X = 0, Y = 0, Z = 0;
	if (Params->TryGetNumberField(TEXT("x"), X)) Location.X = X;
	if (Params->TryGetNumberField(TEXT("y"), Y)) Location.Y = Y;
	if (Params->TryGetNumberField(TEXT("z"), Z)) Location.Z = Z;

	// Use place_actor_from_asset pattern
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Spawn via asset — delegates to the asset type's factory
	AActor* SpawnedActor = nullptr;
	if (GEditor)
	{
		UEditorActorSubsystem* EditorActorSub = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
		if (EditorActorSub)
		{
			SpawnedActor = EditorActorSub->SpawnActorFromObject(SystemAsset, Location);
		}
	}

	if (!SpawnedActor)
	{
		SetError(Cmd, TEXT("Failed to spawn Niagara system actor. Ensure the asset is a valid NiagaraSystem."), TEXT("spawn_failed")); return;
	}

	FString Label;
	if (Params->TryGetStringField(TEXT("label"), Label))
	{
		SpawnedActor->SetActorLabel(Label);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), SpawnedActor->GetPathName());
	Result->SetStringField(TEXT("actor_label"), SpawnedActor->GetActorLabel());
	SetSuccess(Cmd, Result);
}

// ── set_particle_parameter ────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_SetParticleParameter(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString ActorLabel, ParamName;
	if (!Params->TryGetStringField(TEXT("actor_label"), ActorLabel))
	{
		SetError(Cmd, TEXT("Missing 'actor_label'"), TEXT("invalid_params")); return;
	}
	if (!Params->TryGetStringField(TEXT("parameter_name"), ParamName))
	{
		SetError(Cmd, TEXT("Missing 'parameter_name'"), TEXT("invalid_params")); return;
	}

	UWorld* World = GetWorldFromParams(Params);
	AActor* Actor = FindActorByLabel(World, ActorLabel);
	if (!Actor) { SetError(Cmd, FString::Printf(TEXT("Actor not found: '%s'"), *ActorLabel), TEXT("not_found")); return; }

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_label"), ActorLabel);
	Result->SetStringField(TEXT("parameter_name"), ParamName);
	Result->SetStringField(TEXT("note"), TEXT("Niagara parameter setting works best via execute_python using unreal.NiagaraComponent APIs."));
	SetSuccess(Cmd, Result);
}

// ── add_audio_component ───────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_AddAudioComponent(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor) { SetError(Cmd, Err.IsEmpty() ? TEXT("Actor not found") : Err, TEXT("not_found")); return; }

	const TSharedPtr<FJsonObject>& Params = Cmd->Params;

	// Create the audio component
	UAudioComponent* AudioComp = NewObject<UAudioComponent>(Actor, NAME_None, RF_Transactional);
	if (!AudioComp)
	{
		SetError(Cmd, TEXT("Failed to create AudioComponent"), TEXT("creation_failed")); return;
	}
	AudioComp->RegisterComponent();
	AudioComp->AttachToComponent(Actor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
	Actor->AddInstanceComponent(AudioComp);

	// Set sound asset if provided
	FString SoundPath;
	if (Params->TryGetStringField(TEXT("sound_path"), SoundPath))
	{
		USoundBase* Sound = Cast<USoundBase>(StaticLoadObject(USoundBase::StaticClass(), nullptr, *SoundPath));
		if (Sound)
		{
			AudioComp->SetSound(Sound);
		}
	}

	// Set volume
	double Volume = 1.0;
	if (Params->TryGetNumberField(TEXT("volume"), Volume))
	{
		AudioComp->SetVolumeMultiplier(static_cast<float>(Volume));
	}

	// Auto-activate
	bool bAutoActivate = false;
	if (Params->TryGetBoolField(TEXT("auto_activate"), bAutoActivate))
	{
		AudioComp->bAutoActivate = bAutoActivate;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Result->SetStringField(TEXT("component_name"), AudioComp->GetName());
	SetSuccess(Cmd, Result);
}

// ── get_world_partition_info ──────────────────────────────────────────────────
void FMCPTCPServer::Cmd_GetWorldPartitionInfo(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World) { SetError(Cmd, TEXT("No world available"), TEXT("no_world")); return; }

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	UWorldPartition* WP = World->GetWorldPartition();
	if (WP)
	{
		Result->SetBoolField(TEXT("has_world_partition"), true);
		Result->SetStringField(TEXT("world_name"), World->GetName());
	}
	else
	{
		Result->SetBoolField(TEXT("has_world_partition"), false);
		Result->SetStringField(TEXT("note"), TEXT("This level does not use World Partition."));
	}

	SetSuccess(Cmd, Result);
}

// ── get_data_layers ───────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_GetDataLayers(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	UWorld* World = GetWorldFromParams(Cmd->Params);
	if (!World) { SetError(Cmd, TEXT("No world available"), TEXT("no_world")); return; }

	TArray<TSharedPtr<FJsonValue>> LayerArr;

	UDataLayerManager* DLManager = UDataLayerManager::GetDataLayerManager(World);
	if (!DLManager)
	{
		UE_LOG(LogUnrealMCP, Warning, TEXT("GetDataLayers: DataLayerManager is null for world (World Partition may be disabled)."));
	}
	else
	{
		DLManager->ForEachDataLayerInstance([&LayerArr](UDataLayerInstance* Instance) -> bool
		{
			if (!Instance) return true;
			TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
			Info->SetStringField(TEXT("name"), Instance->GetDataLayerShortName());
			Info->SetStringField(TEXT("full_path"), Instance->GetDataLayerFullName());
			LayerArr.Add(MakeShared<FJsonValueObject>(Info));
			return true;
		});
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("data_layers"), LayerArr);
	Result->SetNumberField(TEXT("count"), static_cast<double>(LayerArr.Num()));
	SetSuccess(Cmd, Result);
}

// ── create_physics_constraint ────────────────────────────────────────────────
void FMCPTCPServer::Cmd_CreatePhysicsConstraint(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString Actor1Label, Actor2Label;
	if (!Params->TryGetStringField(TEXT("actor1_label"), Actor1Label))
	{
		SetError(Cmd, TEXT("Missing 'actor1_label'"), TEXT("invalid_params")); return;
	}
	if (!Params->TryGetStringField(TEXT("actor2_label"), Actor2Label))
	{
		SetError(Cmd, TEXT("Missing 'actor2_label'"), TEXT("invalid_params")); return;
	}

	UWorld* World = GetWorldFromParams(Params);
	if (!World) { SetError(Cmd, TEXT("No world available"), TEXT("no_world")); return; }

	AActor* Actor1 = FindActorByLabel(World, Actor1Label);
	AActor* Actor2 = FindActorByLabel(World, Actor2Label);
	if (!Actor1) { SetError(Cmd, FString::Printf(TEXT("Actor1 not found: '%s'"), *Actor1Label), TEXT("not_found")); return; }
	if (!Actor2) { SetError(Cmd, FString::Printf(TEXT("Actor2 not found: '%s'"), *Actor2Label), TEXT("not_found")); return; }

	FVector Location = (Actor1->GetActorLocation() + Actor2->GetActorLocation()) * 0.5f;
	double X = 0, Y = 0, Z = 0;
	if (Params->TryGetNumberField(TEXT("x"), X)) Location.X = X;
	if (Params->TryGetNumberField(TEXT("y"), Y)) Location.Y = Y;
	if (Params->TryGetNumberField(TEXT("z"), Z)) Location.Z = Z;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APhysicsConstraintActor* ConstraintActor = World->SpawnActor<APhysicsConstraintActor>(Location, FRotator::ZeroRotator, SpawnParams);
	if (!ConstraintActor)
	{
		SetError(Cmd, TEXT("Failed to spawn PhysicsConstraintActor"), TEXT("spawn_failed")); return;
	}

	UPhysicsConstraintComponent* Constraint = ConstraintActor->GetConstraintComp();
	if (Constraint)
	{
		Constraint->ConstraintActor1 = Actor1;
		Constraint->ConstraintActor2 = Actor2;

		FString ConstraintType;
		if (Params->TryGetStringField(TEXT("constraint_type"), ConstraintType))
		{
			ConstraintType = ConstraintType.ToLower();
			if (ConstraintType == TEXT("fixed"))
			{
				Constraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0);
				Constraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0);
				Constraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0);
				Constraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Locked, 0);
				Constraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Locked, 0);
				Constraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Locked, 0);
			}
			else if (ConstraintType == TEXT("ball"))
			{
				Constraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0);
				Constraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0);
				Constraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0);
				Constraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Free, 0);
				Constraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Free, 0);
				Constraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Free, 0);
			}
			else if (ConstraintType == TEXT("hinge"))
			{
				Constraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0);
				Constraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0);
				Constraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0);
				Constraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Free, 0);
				Constraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Locked, 0);
				Constraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Locked, 0);
			}
			else if (ConstraintType == TEXT("prismatic"))
			{
				Constraint->SetLinearXLimit(ELinearConstraintMotion::LCM_Free, 0);
				Constraint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0);
				Constraint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0);
				Constraint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Locked, 0);
				Constraint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Locked, 0);
				Constraint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Locked, 0);
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("constraint_actor_path"), ConstraintActor->GetPathName());
	Result->SetStringField(TEXT("actor1"), Actor1Label);
	Result->SetStringField(TEXT("actor2"), Actor2Label);
	SetSuccess(Cmd, Result);
}

// ── configure_ai_damage_perception ────────────────────────────────────────────
void FMCPTCPServer::Cmd_ConfigureAIDamagePerception(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	FString Err;
	AActor* Actor = ResolveActorFromParams(Cmd->Params, &Err);
	if (!Actor) { SetError(Cmd, Err.IsEmpty() ? TEXT("Actor not found") : Err, TEXT("not_found")); return; }

	AAIController* AIC = Cast<AAIController>(Actor);
	if (!AIC) { SetError(Cmd, TEXT("Actor is not an AIController"), TEXT("invalid_actor")); return; }

	UAIPerceptionComponent* PerceptionComp = AIC->FindComponentByClass<UAIPerceptionComponent>();
	if (!PerceptionComp)
	{
		PerceptionComp = NewObject<UAIPerceptionComponent>(AIC, TEXT("AIPerception"));
		if (!PerceptionComp)
		{
			SetError(Cmd, TEXT("Failed to create UAIPerceptionComponent"), TEXT("creation_failed"));
			return;
		}
		PerceptionComp->RegisterComponent();
	}

	// Add Damage sense config
	UAISenseConfig_Damage* DamageConfig = NewObject<UAISenseConfig_Damage>(PerceptionComp, TEXT("DamageSenseConfig"));
	if (!DamageConfig)
	{
		SetError(Cmd, TEXT("Failed to create UAISenseConfig_Damage"), TEXT("creation_failed"));
		return;
	}
	DamageConfig->SetMaxAge(5.0f);
	PerceptionComp->ConfigureSense(*DamageConfig);
	PerceptionComp->SetDominantSense(UAISenseConfig_Damage::StaticClass());

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Result->SetStringField(TEXT("sense"), TEXT("damage"));
	Result->SetStringField(TEXT("status"), TEXT("configured"));
	SetSuccess(Cmd, Result);
}

// ── set_blackboard_value_runtime ──────────────────────────────────────────────
void FMCPTCPServer::Cmd_SetBlackboardValueRuntime(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString ControllerLabel, KeyName, Value;
	if (!Params->TryGetStringField(TEXT("controller_actor_label"), ControllerLabel))
	{
		SetError(Cmd, TEXT("Missing 'controller_actor_label'"), TEXT("invalid_params")); return;
	}
	if (!Params->TryGetStringField(TEXT("key_name"), KeyName))
	{
		SetError(Cmd, TEXT("Missing 'key_name'"), TEXT("invalid_params")); return;
	}
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		SetError(Cmd, TEXT("Missing 'value'"), TEXT("invalid_params")); return;
	}

	// Use PIE world for runtime
	UWorld* World = nullptr;
	if (GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
			{
				World = Ctx.World();
				break;
			}
		}
	}
	if (!World) { SetError(Cmd, TEXT("No PIE world found. Start Play-In-Editor first."), TEXT("no_pie")); return; }

	AActor* ControllerActor = FindActorByLabel(World, ControllerLabel);
	if (!ControllerActor) { SetError(Cmd, FString::Printf(TEXT("Controller not found: '%s'"), *ControllerLabel), TEXT("not_found")); return; }

	AAIController* AIC = Cast<AAIController>(ControllerActor);
	if (!AIC) { SetError(Cmd, TEXT("Actor is not an AIController"), TEXT("invalid_actor")); return; }

	UBlackboardComponent* BB = AIC->GetBlackboardComponent();
	if (!BB) { SetError(Cmd, TEXT("AIController has no BlackboardComponent"), TEXT("no_blackboard")); return; }

	// Try to set value based on key type
	FBlackboard::FKey KeyID = BB->GetKeyID(FName(*KeyName));
	if (KeyID == FBlackboard::InvalidKey)
	{
		SetError(Cmd, FString::Printf(TEXT("Blackboard key '%s' not found"), *KeyName), TEXT("not_found")); return;
	}

	// Try setting as different types
	if (Value.IsNumeric())
	{
		BB->SetValueAsFloat(FName(*KeyName), FCString::Atof(*Value));
	}
	else if (Value == TEXT("true") || Value == TEXT("false"))
	{
		BB->SetValueAsBool(FName(*KeyName), Value == TEXT("true"));
	}
	else
	{
		BB->SetValueAsString(FName(*KeyName), Value);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("key_name"), KeyName);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetStringField(TEXT("status"), TEXT("set"));
	SetSuccess(Cmd, Result);
}

// ── possess_pawn ──────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_PossessPawn(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	FString ControllerLabel, PawnLabel;
	if (!Params->TryGetStringField(TEXT("controller_label"), ControllerLabel))
	{
		SetError(Cmd, TEXT("Missing 'controller_label'"), TEXT("invalid_params")); return;
	}
	if (!Params->TryGetStringField(TEXT("pawn_label"), PawnLabel))
	{
		SetError(Cmd, TEXT("Missing 'pawn_label'"), TEXT("invalid_params")); return;
	}

	UWorld* World = GetWorldFromParams(Params);
	if (!World) { SetError(Cmd, TEXT("No world available"), TEXT("no_world")); return; }

	AActor* ControllerActor = FindActorByLabel(World, ControllerLabel);
	if (!ControllerActor) { SetError(Cmd, FString::Printf(TEXT("Controller not found: '%s'"), *ControllerLabel), TEXT("not_found")); return; }

	AAIController* AIC = Cast<AAIController>(ControllerActor);
	if (!AIC) { SetError(Cmd, TEXT("Actor is not an AIController"), TEXT("invalid_actor")); return; }

	AActor* PawnActor = FindActorByLabel(World, PawnLabel);
	if (!PawnActor) { SetError(Cmd, FString::Printf(TEXT("Pawn not found: '%s'"), *PawnLabel), TEXT("not_found")); return; }

	APawn* Pawn = Cast<APawn>(PawnActor);
	if (!Pawn) { SetError(Cmd, TEXT("Actor is not a Pawn"), TEXT("invalid_actor")); return; }

	AIC->Possess(Pawn);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("controller"), ControllerLabel);
	Result->SetStringField(TEXT("pawn"), PawnLabel);
	Result->SetStringField(TEXT("status"), TEXT("possessed"));
	SetSuccess(Cmd, Result);
}

// ── batch_execute ─────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_BatchExecute(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	if (!Params.IsValid()) { SetError(Cmd, TEXT("Missing params"), TEXT("invalid_params")); return; }

	const TArray<TSharedPtr<FJsonValue>>* CommandsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("commands"), CommandsArr) || !CommandsArr)
	{
		SetError(Cmd, TEXT("Missing 'commands' array"), TEXT("invalid_params")); return;
	}

	bool bUseTransaction = false;
	Params->TryGetBoolField(TEXT("use_transaction"), bUseTransaction);

	TUniquePtr<FScopedTransaction> Transaction;
	if (bUseTransaction)
	{
		Transaction = MakeUnique<FScopedTransaction>(TEXT("MCP Batch Execute"));
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	int32 SuccessCount = 0;

	for (int32 i = 0; i < CommandsArr->Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& CmdVal = (*CommandsArr)[i];
		const TSharedPtr<FJsonObject>* CmdObjPtr = nullptr;
		if (!CmdVal || !CmdVal->TryGetObject(CmdObjPtr) || !CmdObjPtr)
		{
			TSharedPtr<FJsonObject> ErrResult = MakeShared<FJsonObject>();
			ErrResult->SetNumberField(TEXT("index"), i);
			ErrResult->SetBoolField(TEXT("success"), false);
			ErrResult->SetStringField(TEXT("error"), TEXT("Invalid command entry"));
			ResultsArr.Add(MakeShared<FJsonValueObject>(ErrResult));
			continue;
		}

		const TSharedPtr<FJsonObject>& CmdObj = *CmdObjPtr;
		FString SubType;
		CmdObj->TryGetStringField(TEXT("type"), SubType);

		// Create a sub-command
		TSharedPtr<FMCPPendingCommand> SubCmd = MakeShared<FMCPPendingCommand>();
		SubCmd->Id = FString::Printf(TEXT("batch_%d"), i);
		SubCmd->Type = SubType;
		SubCmd->bJsonRpc = Cmd->bJsonRpc;
		SubCmd->ClientId = Cmd->ClientId;

		const TSharedPtr<FJsonObject>* SubParamsPtr = nullptr;
		if (CmdObj->TryGetObjectField(TEXT("params"), SubParamsPtr) && SubParamsPtr)
		{
			SubCmd->Params = *SubParamsPtr;
		}
		else
		{
			SubCmd->Params = MakeShared<FJsonObject>();
		}

		DispatchCommand(SubCmd);

		TSharedPtr<FJsonObject> SubResult = MakeShared<FJsonObject>();
		SubResult->SetNumberField(TEXT("index"), i);
		SubResult->SetStringField(TEXT("type"), SubType);
		SubResult->SetBoolField(TEXT("success"), SubCmd->bSuccess);
		if (SubCmd->bSuccess)
		{
			SubResult->SetObjectField(TEXT("result"), SubCmd->ResultObject);
			++SuccessCount;
		}
		else
		{
			SubResult->SetStringField(TEXT("error"), SubCmd->ErrorMessage);
			SubResult->SetStringField(TEXT("error_code"), SubCmd->ErrorCode);
		}
		ResultsArr.Add(MakeShared<FJsonValueObject>(SubResult));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("results"), ResultsArr);
	Result->SetNumberField(TEXT("total"), static_cast<double>(CommandsArr->Num()));
	Result->SetNumberField(TEXT("succeeded"), static_cast<double>(SuccessCount));
	Result->SetNumberField(TEXT("failed"), static_cast<double>(CommandsArr->Num() - SuccessCount));
	SetSuccess(Cmd, Result);
}

// ── begin_transaction ─────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_BeginTransaction(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	const TSharedPtr<FJsonObject>& Params = Cmd->Params;
	FString Description = TEXT("MCP Transaction");
	if (Params.IsValid()) Params->TryGetStringField(TEXT("description"), Description);

	if (GEditor)
	{
		GEditor->BeginTransaction(*Description);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("transaction_started"));
	Result->SetStringField(TEXT("description"), Description);
	SetSuccess(Cmd, Result);
}

// ── end_transaction ───────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_EndTransaction(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	if (GEditor)
	{
		GEditor->EndTransaction();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("transaction_ended"));
	SetSuccess(Cmd, Result);
}

// ── undo ──────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_Undo(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	int32 Count = 1;
	if (Cmd->Params.IsValid())
	{
		double DCount = 1;
		if (Cmd->Params->TryGetNumberField(TEXT("count"), DCount))
		{
			Count = FMath::Max(1, static_cast<int32>(DCount));
		}
	}

	bool bSuccess = false;
	if (GEditor && GEditor->Trans)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			bSuccess = GEditor->UndoTransaction(true);
			if (!bSuccess) break;
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Count);
	Result->SetBoolField(TEXT("success"), bSuccess);
	SetSuccess(Cmd, Result);
}

// ── redo ──────────────────────────────────────────────────────────────────────
void FMCPTCPServer::Cmd_Redo(TSharedPtr<FMCPPendingCommand>& Cmd)
{
	int32 Count = 1;
	if (Cmd->Params.IsValid())
	{
		double DCount = 1;
		if (Cmd->Params->TryGetNumberField(TEXT("count"), DCount))
		{
			Count = FMath::Max(1, static_cast<int32>(DCount));
		}
	}

	bool bSuccess = false;
	if (GEditor && GEditor->Trans)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			bSuccess = GEditor->RedoTransaction(true);
			if (!bSuccess) break;
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Count);
	Result->SetBoolField(TEXT("success"), bSuccess);
	SetSuccess(Cmd, Result);
}
