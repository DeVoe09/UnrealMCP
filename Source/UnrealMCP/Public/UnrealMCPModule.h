// Copyright CustomUnrealMCP. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FMCPTCPServer;

// ── Log category ─────────────────────────────────────────────────────────────
DECLARE_LOG_CATEGORY_EXTERN(LogUnrealMCP, Log, All);

// ── Module interface ──────────────────────────────────────────────────────────
class UNREALMCP_API FUnrealMCPModule : public IModuleInterface
{
public:
	/** IModuleInterface */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Singleton accessor */
	static FUnrealMCPModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FUnrealMCPModule>("UnrealMCP");
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("UnrealMCP");
	}

	/** Returns true if the TCP server is running */
	bool IsServerRunning() const;

	/** Restart the TCP server (e.g. after a port change) */
	void RestartServer();

	/** Expose server pointer so the toolbar widget can poll it */
	FMCPTCPServer* GetTCPServer() const { return TCPServer.Get(); }

private:
	TUniquePtr<FMCPTCPServer> TCPServer;

	/** Registers the MCP status widget into the Level Editor toolbar */
	void RegisterMenus();
};
