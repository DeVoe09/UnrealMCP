// Copyright CustomUnrealMCP. All Rights Reserved.

#include "MCPStatusWidget.h"
#include "MCPTCPServer.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SOverlay.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Fonts/SlateFontInfo.h"

#define LOCTEXT_NAMESPACE "UnrealMCP"

// ── Indicator colours ─────────────────────────────────────────────────────────
namespace MCPColors
{
	static const FLinearColor Green  (0.00f, 0.85f, 0.30f, 1.f);  // Connected
	static const FLinearColor Amber  (1.00f, 0.65f, 0.00f, 1.f);  // Waiting
	static const FLinearColor Red    (0.90f, 0.10f, 0.10f, 1.f);  // Failed
	static const FLinearColor TextOn (0.95f, 0.95f, 0.95f, 1.f);  // Label when connected
	static const FLinearColor TextOff(0.55f, 0.55f, 0.55f, 1.f);  // Label when not connected
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

void SMCPStatusWidget::Construct(const FArguments& InArgs)
{
	Server = InArgs._Server;

	// Pulse animation: 1.5-second looping sinusoid for the dot when connected
	PulseSequence = FCurveSequence(0.f, 1.5f, ECurveEaseFunction::CubicInOut);
	PulseCurve    = PulseSequence.AddCurve(0.f, 1.5f, ECurveEaseFunction::CubicInOut);
	PulseSequence.Play(this->AsShared(), true /*loop*/);

	ChildSlot
	.VAlign(VAlign_Center)
	.Padding(FMargin(6.f, 0.f))
	[
		SNew(SHorizontalBox)

		// ── Coloured dot ──────────────────────────────────────────────────────
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(12.f)
			.HeightOverride(12.f)
			.ToolTipText_Lambda([this]() { return GetTooltipText(); })
			[
				// Outer ring (always solid, shows the base colour slightly dimmed)
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("\u25CF")))   // ● BULLET (U+25CF)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 12))
					.ColorAndOpacity_Lambda([this]() -> FSlateColor
					{
						FLinearColor C = GetDotColor().GetSpecifiedColor();
						C.A = 0.35f;   // dimmed ring
						return FSlateColor(C);
					})
				]
				// Inner dot (pulses opacity when connected)
				+ SOverlay::Slot()
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("\u25CF")))   // ●
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
					.ColorAndOpacity_Lambda([this]() -> FSlateColor
					{
						FLinearColor C = GetDotColor().GetSpecifiedColor();
						C.A = GetDotOpacity();
						return FSlateColor(C);
					})
				]
			]
		]

		// ── "MCP" label ───────────────────────────────────────────────────────
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("MCPLabel", "MCP"))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity_Lambda([this]() -> FSlateColor
			{
				const EMCPConnectionState State = GetState();
				return FSlateColor(State == EMCPConnectionState::Connected
					? MCPColors::TextOn
					: MCPColors::TextOff);
			})
			.ToolTipText_Lambda([this]() { return GetTooltipText(); })
		]
	];
}

// ─────────────────────────────────────────────────────────────────────────────
// Attribute callbacks
// ─────────────────────────────────────────────────────────────────────────────

EMCPConnectionState SMCPStatusWidget::GetState() const
{
	if (!Server) return EMCPConnectionState::ServerFailed;
	return Server->GetConnectionState();
}

FSlateColor SMCPStatusWidget::GetDotColor() const
{
	switch (GetState())
	{
		case EMCPConnectionState::Connected:   return FSlateColor(MCPColors::Green);
		case EMCPConnectionState::Waiting:     return FSlateColor(MCPColors::Amber);
		case EMCPConnectionState::ServerFailed:
		default:                               return FSlateColor(MCPColors::Red);
	}
}

float SMCPStatusWidget::GetDotOpacity() const
{
	// When connected: pulse between 0.6 and 1.0 using the curve
	if (GetState() == EMCPConnectionState::Connected)
	{
		const float T = PulseCurve.GetLerp();   // 0..1
		return FMath::Lerp(0.55f, 1.0f, T);
	}
	// Otherwise static
	return GetState() == EMCPConnectionState::Waiting ? 0.9f : 0.7f;
}

FText SMCPStatusWidget::GetTooltipText() const
{
	if (!Server)
	{
		return LOCTEXT("MCPTooltipNoServer", "Unreal MCP — server not initialised");
	}
	return FText::FromString(Server->GetStatusString());
}

FText SMCPStatusWidget::GetLabelText() const
{
	return LOCTEXT("MCPLabel", "MCP");
}

#undef LOCTEXT_NAMESPACE
