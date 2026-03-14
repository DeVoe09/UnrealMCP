# AI Commands Implementation Plan

**Status: Implemented as of UnrealMCP 1.1.0.** See README Command Reference — AI for the current command list.

> **For Claude:** Use this plan to implement the AI command set task-by-task. Follow existing patterns in `MCPTCPServer.cpp` (asset creation, graph nodes, property editing).

**Goal:** Extend the MCP from "scene builder" to "gameplay orchestrator" by adding native commands for Behavior Trees, Blackboards, AI Controller/Perception, and Navigation.

**Architecture:** Reuse existing patterns: new C++ handlers in `FMCPTCPServer`, dispatch in `DispatchCommand`, JSON params/result. Cast/resolve assets (UBehaviorTree, UBlackboardData) and actors (AIController, Pawn) similarly to Blueprint and actor resolution. Editor-only for asset creation/graph editing; runtime-facing commands (run_behavior_tree, set_blackboard_value at runtime) require world/controller from params.

**Tech Stack:** UE5 AIModule, GameplayTasks, BehaviorTreeEditor (editor), NavigationSystem. Existing: UnrealEd, AssetTools, AssetRegistry, Json.

---

## Module Dependencies

**File:** `Source/UnrealMCP/UnrealMCP.Build.cs`

Add to `PrivateDependencyModuleNames` (all are available in Editor builds):

- `"AIModule"` — UBehaviorTree, UBlackboardData, UBlackboardKeyType*, FBlackboardEntry, AAIController, UAIPerceptionComponent
- `"GameplayTasks"` — Used by Behavior Tree runtime
- `"NavigationSystem"` — UNavigationSystemV1::Build()
- `"BehaviorTreeEditor"` — UBehaviorTreeGraph, UBehaviorTreeGraphNode_*, FEdGraphSchemaAction (BT schema), creation of BT graph nodes

**Note:** BehaviorTreeEditor is an Editor module; the plugin is already Editor-only for the TCP server. No need for runtime-only builds.

---

## Command Set Summary

| Command | Category | Purpose |
|---------|----------|---------|
| `create_behavior_tree` | Assets | Create UBehaviorTree + UBlackboardData pair (optionally link BB to BT). |
| `add_bt_node` | Logic | Add Composite (Selector/Sequence), Task, Decorator, or Service to a BT graph. |
| `add_blackboard_key` | Memory | Add a named key (Vector, Object, Float, Int, Bool, String, etc.) to a UBlackboardData asset. |
| `run_behavior_tree` | Brain | Assign and run a UBehaviorTree on an AI Controller (actor_path/label + world params). |
| `configure_ai_perception` | Senses | Configure UAIPerceptionComponent on an AI Controller (sight/hearing radius, add senses). |
| `rebuild_navigation` | Integration | Call UNavigationSystemV1::Build() for the editor world (so newly placed obstacles are recognized). |

Optional (can be Phase 2):

- `possess_pawn` — Link an AI Controller to a Pawn (Possess(Pawn)).
- `set_blackboard_value` — At runtime (PIE), set a blackboard key value on a specific controller (requires resolving controller + key name + value JSON).

---

## Task 1: Build and header wiring

**Files:**
- Modify: `Source/UnrealMCP/UnrealMCP.Build.cs`
- Modify: `Source/UnrealMCP/Public/MCPTCPServer.h`

**Steps:**

1. Add `AIModule`, `GameplayTasks`, `NavigationSystem`, `BehaviorTreeEditor` to `PrivateDependencyModuleNames` in `UnrealMCP.Build.cs`.
2. In `MCPTCPServer.h`, add declarations for the six handlers (and optional `possess_pawn` / `set_blackboard_value` if desired):
   - `void Cmd_CreateBehaviorTree(TSharedPtr<FMCPPendingCommand>& Cmd);`
   - `void Cmd_AddBTNode(TSharedPtr<FMCPPendingCommand>& Cmd);`
   - `void Cmd_AddBlackboardKey(TSharedPtr<FMCPPendingCommand>& Cmd);`
   - `void Cmd_RunBehaviorTree(TSharedPtr<FMCPPendingCommand>& Cmd);`
   - `void Cmd_ConfigureAIPerception(TSharedPtr<FMCPPendingCommand>& Cmd);`
   - `void Cmd_RebuildNavigation(TSharedPtr<FMCPPendingCommand>& Cmd);`
3. In `MCPTCPServer.cpp`, in `DispatchCommand`, add `else if (Type == TEXT("create_behavior_tree")) Cmd_CreateBehaviorTree(Cmd);` (and same for the other five). Implement each handler as a stub that calls `SetError(Cmd, TEXT("Not implemented yet"));` so the project compiles.
4. Build the plugin (EmoEditor Win64 Development). Fix any missing include or link errors (e.g. correct module name if BehaviorTreeEditor differs in your engine version).

---

## Task 2: create_behavior_tree

**Params:** `asset_name`, `package_path` [, `blackboard_name` — optional name for the Blackboard asset, same package ]

**Result:** `behavior_tree_path`, `blackboard_path` (object paths of created assets).

**Key APIs:**
- Create UBlackboardData via AssetTools or factory (e.g. `UBlackboardDataFactory` if available) or `NewObject<UBlackboardData>` and register with AssetRegistry.
- Create UBehaviorTree: same pattern (factory or NewObject). Set `UBehaviorTree::BlackboardAsset` to the new UBlackboardData.
- Save assets and return paths.

**Implementation notes:**
- UE may use `UBehaviorTreeFactory` / `UBlackboardDataFactory`; check `IAssetTools::GetAssetFactories()` or engine source `BehaviorTreeEditor` module for how the editor creates these assets. If no public factory, use `NewObject` and `FAssetRegistryModule::AssetCreated()` / package save.
- **Files:** `MCPTCPServer.cpp` (implement `Cmd_CreateBehaviorTree`), include `BehaviorTree/BehaviorTree.h`, `BehaviorTree/BlackboardData.h`, and any factory headers.

---

## Task 3: add_blackboard_key

**Params:** `blackboard_path` (asset or object path), `key_name`, `key_type` (e.g. `"Vector"`, `"Object"`, `"Float"`, `"Int"`, `"Bool"`, `"String"`, `"Enum"` [, `enum_path` ]).

**Result:** `key_id` (FBlackboardKeySelector compatible id or name).

**Key APIs:**
- Load/find UBlackboardData by path (AssetRegistry or LoadObject).
- `UBlackboardData::Keys` — array of `FBlackboardEntry`. Create `UBlackboardKeyType*` (e.g. `UBlackboardKeyType_Vector`, `UBlackboardKeyType_Object`) via `NewObject`, then add `FBlackboardEntry` with `EntryName` and `KeyType`.
- Mark asset dirty and optionally save.

**Implementation notes:**
- Map `key_type` string to `UBlackboardKeyType*` subclass. Common: Vector, Object, Float, Int, Bool, String. Enum requires `enum_path` and `UBlackboardKeyType_Enum`.
- **Files:** `MCPTCPServer.cpp` (implement `Cmd_AddBlackboardKey`), includes for `BlackboardData.h`, `BlackboardKeyType.h`, and key type classes (`BlackboardKeyType_Vector.h`, etc.).

---

## Task 4: add_bt_node

**Params:** `behavior_tree_path` (or `behavior_tree_name` resolved like Blueprint), `node_type` (`"Composite"` | `"Task"` | `"Decorator"` | `"Service"`), `node_class` (e.g. `"BTComposite_Selector"`, `"BTTask_Wait"`, or Blueprint task class path) [, `parent_node_id` ], `node_position` [x,y].

**Result:** `node_id` (graph node GUID or equivalent).

**Key APIs:**
- Resolve UBehaviorTree asset. Access the BT graph: `UBehaviorTree` has an editor graph (e.g. `UBehaviorTree::GetGraph()` or similar — check engine; may be in `UEdGraph*` stored on the asset or via `UBehaviorTreeGraph`).
- BehaviorTreeEditor: `UBehaviorTreeGraph`, `UBehaviorTreeGraphNode_Root`, `UBehaviorTreeGraphNode_Composite`, `UBehaviorTreeGraphNode_Task`, etc. Create the appropriate graph node with `NewObject`, set parent if needed, add to graph, then create the underlying `UBTNode` (e.g. `UBTComposite_Selector`) and link.
- Engine pattern: Often the graph node wraps a `UBTNode`. Check `UBehaviorTreeGraphNode::NodeInstance` or similar. Create node via schema action if possible (e.g. `FBehaviorTreeSchemaAction_*::PerformAction`) to match editor behavior.

**Implementation notes:**
- This is the most complex command. Fallback: use `FEdGraphSchemaAction_NewNode`-style creation if the BehaviorTreeEditor exposes schema actions for "Add Selector", "Add Task", etc. Alternatively, spawn the appropriate `UBehaviorTreeGraphNode_*` and the corresponding runtime `UBTNode` (e.g. `UBTComposite_Selector`, `UBTTask_Wait`) and wire them.
- **Files:** `MCPTCPServer.cpp`, includes from BehaviorTreeEditor (e.g. `BehaviorTreeGraph.h`, `BehaviorTreeGraphNode.h`, `EdGraphSchema_BehaviorTree.h`).

---

## Task 5: run_behavior_tree

**Params:** `controller_actor_path` | `controller_actor_label`, `behavior_tree_path` | `behavior_tree_name` [, world params: `use_pie`, `world_context_index` ].

**Result:** `success` (bool). Optional: `message` if the controller is not an AAIController or RunBehaviorTree fails.

**Key APIs:**
- Resolve world from params (reuse `GetWorldFromParams`).
- Resolve actor (AIController) via `ResolveActorFromParams` or by label/path in that world.
- Cast to `AAIController*`. Load UBehaviorTree from path/name. Call `AAIController::RunBehaviorTree(UBehaviorTree* BT)`.
- **Note:** This affects runtime (PIE or game). In editor, if no PIE, you might run on the "preview" or return an error that PIE is required for run_behavior_tree.

**Files:** `MCPTCPServer.cpp`, include `AIController.h`, `BehaviorTree/BehaviorTree.h`. Reuse existing world/actor resolution.

---

## Task 6: configure_ai_perception

**Params:** `controller_actor_path` | `controller_actor_label`, [ `sight_radius`, `sight_age`, `hearing_radius`, `add_senses` (array of strings, e.g. `["Sight","Hearing"]`) ] [, world params ].

**Result:** `success`. Optional: list of configured sense IDs.

**Key APIs:**
- Resolve AI Controller (same as Task 5). Get `UAIPerceptionComponent*` from the controller (e.g. `AAIController::GetPerceptionComponent()` or component by class).
- Configure senses: add/configure `UAISenseConfig_*` (Sight, Hearing, Damage). Set radius, max age, etc. `UAIPerceptionComponent::ConfigureSense(*Config)` or equivalent.
- **Files:** `MCPTCPServer.cpp`, include `Perception/AIPerceptionComponent.h`, `Perception/AISenseConfig_*.h`.

---

## Task 7: rebuild_navigation

**Params:** [ world params; default editor world ]

**Result:** `success`, optional `message` (e.g. "Navigation built for World X").

**Key APIs:**
- Get world from params (default: GEditor->GetEditorWorldContext().World()).
- `UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);` then `NavSys->Build()` (or the correct rebuild API for your engine version — e.g. `Build()` or `RebuildAll`).
- **Files:** `MCPTCPServer.cpp`, include `NavigationSystem.h`, `NavMesh/NavigationSystem.h` as needed.

---

## Task 8: Documentation and audit

**Files:**
- Modify: `Plugins/UnrealMCPPlugin/README.md` — add new commands under an "AI (Behavior Tree, Blackboard, Perception, Navigation)" section with params and descriptions.
- Modify: `Plugins/UnrealMCPPlugin/AUDIT_LIMITATIONS.md` — add a new subsection under a suitable section (e.g. "AI & behavior") stating that create_behavior_tree, add_bt_node, add_blackboard_key, run_behavior_tree, configure_ai_perception, rebuild_navigation are supported; note any limitations (e.g. run_behavior_tree requires PIE for runtime effect).

---

## Optional (Phase 2)

- **possess_pawn:** Params: `controller_actor_path`, `pawn_actor_path` (+ world). Call `AIController->Possess(Pawn)`. Return success.
- **set_blackboard_value:** Params: `controller_actor_path`, `key_name`, `value` (JSON) (+ world). Resolve controller, get blackboard component, set key by name. Requires runtime blackboard (UBlackboardComponent) and key type-aware value import (similar to set_actor_property).

---

## Testing (manual)

1. **create_behavior_tree:** Call via MCP client; verify assets in Content Browser; open BT and confirm Blackboard is linked.
2. **add_blackboard_key:** Add a key to the created Blackboard; reopen asset and confirm key appears.
3. **add_bt_node:** Add a Selector and a Task to the BT graph; open in editor and confirm nodes exist and compile.
4. **run_behavior_tree:** Place an AIController and Pawn in level, run PIE, call run_behavior_tree with controller and BT path; confirm BT runs (e.g. via debug or visual feedback).
5. **configure_ai_perception:** Set sight radius on a controller; verify in editor or PIE that perception config is updated.
6. **rebuild_navigation:** Place a NavMeshBoundsVolume, call rebuild_navigation; verify nav mesh rebuilds (e.g. in editor viewport or PIE).

---

## Order of implementation

1. Task 1 (deps + stubs) — unblocks compile and dispatch.
2. Task 7 (rebuild_navigation) — smallest surface, no BT editor dependency.
3. Task 2 (create_behavior_tree) — foundational assets.
4. Task 3 (add_blackboard_key) — memory layer.
5. Task 4 (add_bt_node) — highest complexity; can start with a single node type (e.g. Selector) then expand.
6. Task 5 (run_behavior_tree) — brain layer.
7. Task 6 (configure_ai_perception) — senses.
8. Task 8 (docs).

This order keeps each step buildable and testable.
