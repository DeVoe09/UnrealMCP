// Copyright CustomUnrealMCP. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "MCPTCPServer.h"

// ─────────────────────────────────────────────────────────────────────────────
// SMCPStatusWidget
//
// Toolbar indicator showing the current MCP connection state:
//
//   🔴  ● MCP   — server failed to start (port in use?)
//   🟡  ● MCP   — server running, waiting for Claude Code
//   🟢  ● MCP   — Claude Code is connected
//
// Pulsing animation on the dot when connected. Tooltip shows full status.
// ─────────────────────────────────────────────────────────────────────────────
class UNREALMCP_API SMCPStatusWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMCPStatusWidget)
		: _Server(nullptr)
	{}
		/** Pointer to the TCP server (owned by the module, outlives this widget) */
		SLATE_ARGUMENT(FMCPTCPServer*, Server)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FMCPTCPServer* Server = nullptr;

	// Animation curve for the "connected" pulse effect
	FCurveSequence PulseSequence;
	FCurveHandle   PulseCurve;

	// Attribute callbacks (bound to Slate attributes — called every frame)
	FSlateColor GetDotColor()    const;
	FText       GetTooltipText() const;
	FText       GetLabelText()   const;
	float       GetDotOpacity()  const;

	// Convenience
	EMCPConnectionState GetState() const;
};
