// Copyright CustomUnrealMCP. All Rights Reserved.

using UnrealBuildTool;

public class UnrealMCP : ModuleRules
{
	public UnrealMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// ── Core dependencies ─────────────────────────────────────────────────
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		// ── Editor-only dependencies ──────────────────────────────────────────
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Editor fundamentals
			"UnrealEd",
			"Slate",
			"SlateCore",
			"EditorStyle",

			// Networking (TCP server)
			"Networking",
			"Sockets",

			// JSON (command protocol)
			"Json",
			"JsonUtilities",

			// Editor subsystems
			"EditorSubsystem",
			"EditorScriptingUtilities",   // UEditorLevelLibrary, UEditorActorSubsystem

			// Asset system
			"AssetRegistry",
			"AssetTools",
			"ContentBrowser",   // IContentBrowserSingleton::GetSelectedAssets

			// Blueprint
			"BlueprintEditorLibrary",
			"KismetCompiler",
			"BlueprintGraph",          // UK2Node_Event, UK2Node_CallFunction, EdGraphSchema_K2

			// Level editor + toolbar extension
			"LevelEditor",
			"ToolMenus",          // UToolMenus — required for toolbar widget registration

			// DeveloperSettings (for plugin config)
			"DeveloperSettings",

			// Material editing (connect expressions, add expression, recompile)
			"MaterialEditor",

			// UMG (Widget Blueprint, WidgetTree, add widget)
			"UMG",
			"UMGEditor",   // UWidgetBlueprint

			// AI (Behavior Tree, Blackboard, AI Controller, Perception, Navigation)
			"AIModule",
			"GameplayTasks",
			"AIGraph",             // Required by BehaviorTreeGraph.h (UBehaviorTreeGraph, FGraphNodeCreator)
			"BehaviorTreeEditor",   // UBehaviorTreeFactory, UBlackboardDataFactory
			"NavigationSystem",    // UNavigationSystemV1::Build()
		});

		// ── Optional: Python Script Plugin ───────────────────────────────────
		// Enables ue_execute_python — the "god-mode" tool.
		// If PythonScriptPlugin is not present the plugin still works,
		// execute_python will return a "Python not available" error instead.
		if (Target.bBuildEditor)
		{
			// PythonScriptPlugin is always present in UE Editor builds (5.0+)
			PrivateDependencyModuleNames.Add("PythonScriptPlugin");
		}
	}
}
