// Copyright CustomUnrealMCP. All Rights Reserved.

#include "UnrealMCPModule.h"
#include "MCPTCPServer.h"
#include "MCPStatusWidget.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

IMPLEMENT_MODULE(FUnrealMCPModule, UnrealMCP)

DEFINE_LOG_CATEGORY(LogUnrealMCP);

// ── [UnrealMCP] config: project DefaultEngine.ini (GGameIni) overrides engine ─
// Port=55557, CommandTimeoutSeconds=30, MaxCommandsPerTick=16
static int32 GetConfiguredPort()
{
	int32 Port = 55557;
	GConfig->GetInt(TEXT("UnrealMCP"), TEXT("Port"), Port, GEngineIni);
	GConfig->GetInt(TEXT("UnrealMCP"), TEXT("Port"), Port, GGameIni);
	return Port;
}

static int32 GetConfiguredCommandTimeoutSeconds()
{
	int32 Sec = 30;
	GConfig->GetInt(TEXT("UnrealMCP"), TEXT("CommandTimeoutSeconds"), Sec, GEngineIni);
	GConfig->GetInt(TEXT("UnrealMCP"), TEXT("CommandTimeoutSeconds"), Sec, GGameIni);
	return FMath::Clamp(Sec, 5, 300);
}

static int32 GetConfiguredMaxCommandsPerTick()
{
	int32 N = 16;
	GConfig->GetInt(TEXT("UnrealMCP"), TEXT("MaxCommandsPerTick"), N, GEngineIni);
	GConfig->GetInt(TEXT("UnrealMCP"), TEXT("MaxCommandsPerTick"), N, GGameIni);
	return FMath::Clamp(N, 1, 256);
}

static int32 GetConfiguredMaxRequestLineBytes()
{
	int32 Bytes = 16 * 1024 * 1024;
	GConfig->GetInt(TEXT("UnrealMCP"), TEXT("MaxRequestLineBytes"), Bytes, GEngineIni);
	GConfig->GetInt(TEXT("UnrealMCP"), TEXT("MaxRequestLineBytes"), Bytes, GGameIni);
	return FMath::Clamp(Bytes, 4096, 64 * 1024 * 1024);
}

static bool GetConfiguredRateLimitEnabled()
{
	bool bEnabled = false;
	GConfig->GetBool(TEXT("UnrealMCP"), TEXT("RateLimitEnabled"), bEnabled, GEngineIni);
	GConfig->GetBool(TEXT("UnrealMCP"), TEXT("RateLimitEnabled"), bEnabled, GGameIni);
	return bEnabled;
}

// ─────────────────────────────────────────────────────────────────────────────

void FUnrealMCPModule::StartupModule()
{
	UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP module starting up"));

	TCPServer = MakeUnique<FMCPTCPServer>();
	const int32 Port = GetConfiguredPort();
	const int32 TimeoutSec = GetConfiguredCommandTimeoutSeconds();
	const int32 MaxCommandsPerTick = GetConfiguredMaxCommandsPerTick();
	const int32 MaxRequestLineBytes = GetConfiguredMaxRequestLineBytes();

	if (TCPServer->Start(Port, TimeoutSec, MaxCommandsPerTick, MaxRequestLineBytes, GetConfiguredRateLimitEnabled()))
	{
		UE_LOG(LogUnrealMCP, Log,
			TEXT("UnrealMCP TCP server listening on port %d. Claude Code can now connect."), Port);
	}
	else
	{
		UE_LOG(LogUnrealMCP, Error,
			TEXT("UnrealMCP failed to start TCP server on port %d. "
			     "Is another process already using that port?"), Port);
	}

	// Register the toolbar status widget once UToolMenus is ready.
	// UToolMenus::RegisterStartupCallback fires after all modules are loaded
	// (including LevelEditor) so the toolbar already exists.
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(
			this, &FUnrealMCPModule::RegisterMenus));
}

void FUnrealMCPModule::ShutdownModule()
{
	UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP module shutting down"));

	// Unregister all UToolMenus extensions owned by this module
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	if (TCPServer)
	{
		TCPServer->Stop();
		TCPServer.Reset();
	}
}

bool FUnrealMCPModule::IsServerRunning() const
{
	return TCPServer && TCPServer->IsRunning();
}

void FUnrealMCPModule::RestartServer()
{
	if (TCPServer)
	{
		TCPServer->Stop();
		TCPServer->Start(GetConfiguredPort(), GetConfiguredCommandTimeoutSeconds(), GetConfiguredMaxCommandsPerTick(), GetConfiguredMaxRequestLineBytes(), GetConfiguredRateLimitEnabled());
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Toolbar registration
// ─────────────────────────────────────────────────────────────────────────────

void FUnrealMCPModule::RegisterMenus()
{
	// Set the owner so UToolMenus can clean up when the module unloads
	FToolMenuOwnerScoped OwnerScoped(this);

	// ── Extend the Level Editor Play toolbar ─────────────────────────────────
	// "LevelEditor.LevelEditorToolBar.PlayToolBar" is the right-side section
	// of the main Level Editor toolbar (next to the Play/Simulate buttons).
	// This is where Special Agent and similar plugins place their indicators.
	UToolMenu* ToolBar = UToolMenus::Get()->ExtendMenu(
		"LevelEditor.LevelEditorToolBar.PlayToolBar");

	if (!ToolBar)
	{
		UE_LOG(LogUnrealMCP, Warning,
			TEXT("RegisterMenus: could not find LevelEditorToolBar.PlayToolBar"));
		return;
	}

	FToolMenuSection& Section = ToolBar->FindOrAddSection("UnrealMCP");
	Section.Label = NSLOCTEXT("UnrealMCP", "MCPSection", "MCP");

	// Add a custom widget entry — the coloured dot + "MCP" label
	FMCPTCPServer* ServerPtr = TCPServer.Get();   // raw pointer captured in lambda

	Section.AddEntry(FToolMenuEntry::InitWidget(
		"MCPStatusWidget",
		SNew(SMCPStatusWidget)
			.Server(ServerPtr),
		FText::GetEmpty(),   // no label beside the widget
		/*bNoIndent=*/ true,
		/*bNoPadding=*/ false
	));

	UE_LOG(LogUnrealMCP, Log, TEXT("UnrealMCP status indicator registered in Level Editor toolbar"));
}
