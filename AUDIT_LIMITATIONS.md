# UnrealMCP Plugin — Audit: Remaining Limitations

**Audit scope:** `Plugins/UnrealMCPPlugin` — native MCP commands and behavior.  
**Count:** 102 command types (including `ping`, `get_status`, and 7 AI commands). See **README.md** for the full command list.  
**Last audit:** Post–AI suite + Hearing + BT Composite (Selector/Sequence) + status/works audit.  
**Remaining gaps in tables below:** 0 (all capabilities below have native implementations; see §12 for workarounds only where needed).

---

## 1. Outliner & folders

| Capability | Status | Note |
|------------|--------|------|
| Read actor folder | ✅ | `get_all_actors` returns `folder_path` (and `actor_path`) |
| Set actor folder / move to folder | ✅ | `set_actor_folder` (actor_path/label + folder_path), `set_selected_actors_folder` (folder_path) |
| List folders / create folder | ✅ | `list_actor_folders` (flat list of paths), `create_actor_folder` (folder_path + world params) |

---

## 2. Content Browser & assets

| Capability | Status | Note |
|------------|--------|------|
| List assets (path, class, name) | ✅ | `get_assets` with path/class filter |
| Full asset metadata + tags | ✅ | `get_asset_full_metadata` |
| Save / delete / duplicate / rename asset | ✅ | `save_asset`, `delete_asset`, `duplicate_asset`, `rename_asset` |
| Create Blueprint | ✅ | `create_blueprint` (from class) |
| Create Material | ✅ | `create_material` |
| Create arbitrary asset type | ✅ | `create_asset` with factory discovery; DataTable requires `row_struct` (UScriptStruct path); Texture/others via factory. |
| List content subpaths (folder hierarchy) | ✅ | `get_content_subpaths` (base_path, recursive) |
| Create content browser folder | ✅ | `create_content_folder` (folder_path e.g. /Game/MyFolder) |
| Get/set content browser selection | ✅ | `get_selected_assets`, `set_selected_assets` (asset_paths array) |

---

## 3. Blueprint graph

| Capability | Status | Note |
|------------|--------|------|
| List graphs, find nodes, get node info (pins, position) | ✅ | `get_blueprint_graphs`, `find_blueprint_nodes`, `get_node_info` |
| Add Event / CallFunction / Branch / Sequence / Switch (int) / Variable nodes | ✅ | Plus connect, compile, set position, set pin default, delete node, disconnect pins |
| Find graph by name (incl. interface graphs) | ✅ | `FindGraphByName` includes `ImplementedInterfaces[].Graphs` |
| Find node by GUID in interface graphs | ✅ | `get_node_info`, `find_blueprint_nodes`, and all node ops search DelegateSignatureGraphs and ImplementedInterfaces |
| Add Timeline / Gate / MultiGate / ForEach / Switch (string/enum) | ✅ | `add_blueprint_timeline_node`, `add_blueprint_foreach_node`, `add_blueprint_switch_string_node`, `add_blueprint_switch_enum_node`, `add_blueprint_gate_node`, `add_blueprint_multigate_node` |
| Create new graph (function or macro) | ✅ | `create_blueprint_graph` (blueprint_name, graph_name, graph_type: "function" \| "macro") |
| Get blueprint pins for a node | ✅ | `get_node_info` returns `pins` (name, direction, type, connected_to) |

---

## 4. Materials

| Capability | Status | Note |
|------------|--------|------|
| Create material, list expressions | ✅ | `create_material`, `get_material_expressions` |
| Add expression, connect, connect to property, recompile | ✅ | `add_material_expression`, `connect_material_expressions`, `connect_material_property`, `recompile_material` |
| Delete material expression | ✅ | `delete_material_expression` (material_path, expression_guid or expression_name; optional recompile) |
| Get expression input/output pin names | ✅ | `get_material_expression_pins` (material_path, expression_guid or expression_name) returns input_names and input_types |

---

## 5. UMG (Widget Blueprint)

| Capability | Status | Note |
|------------|--------|------|
| Add widget to tree (root or named parent) | ✅ | `add_umg_widget` (widget_class, parent_widget_name, widget_name) |
| Remove widget from tree | ✅ | `remove_umg_widget` (widget_blueprint_path, widget_name) |
| Set named slot content | ✅ | `set_umg_slot_content` (widget_blueprint_path, slot_name, content_widget_name) |
| Get widget tree (hierarchy) | ✅ | `get_umg_tree` (widget_blueprint_path) returns root with name, class, children |
| Create Widget Blueprint | ✅ | `create_widget_blueprint` (asset_name, package_path [, parent_class ]) |

---

## 6. Level & world

| Capability | Status | Note |
|------------|--------|------|
| Open level | ✅ | `open_level` |
| World context (editor vs PIE) | ✅ | `use_pie`, `world_context_index` on get_all_actors, spawn_actor, duplicate_actor, console_command; actor resolution uses same world |
| Get current level name/path | ✅ | `get_current_level` (optional world params) returns level_name, map_name, filename, path |
| List loaded levels / streaming | ✅ | `get_loaded_levels` returns levels (name, path, is_visible) and streaming (package_name, package_path, is_loaded) |
| List world contexts (e.g. valid indices) | ✅ | `get_world_contexts` returns index, world_type, has_world, path per context |
| Load / unload streaming level (editor) | ✅ | `load_streaming_level` (level_path), `unload_streaming_level` (level_path or package_name); editor world only |

---

## 7. Editor selection & viewport

| Capability | Status | Note |
|------------|--------|------|
| Focus viewport (camera position/rotation) | ✅ | `focus_viewport` (optional x,y,z, pitch, yaw) |
| Screenshot | ✅ | `take_screenshot` |
| Get selected actors | ✅ | `get_selected_actors` returns actors (actor_path, actor_label, class) |
| Set selected actors | ✅ | `set_selected_actors` (actor_paths or actor_labels array + world params) |
| Get viewport camera transform | ✅ | `get_viewport_transform` (optional viewport_index) returns location, rotation, fov |
| Set viewport FOV (without moving camera) | ✅ | `set_viewport_fov` (fov in degrees, optional viewport_index) |
| List all open viewports | ✅ | `list_viewports` — returns raw_index, perspective_index, is_perspective, fov, location, rotation per viewport |

---

## 8. Properties (get/set)

| Capability | Status | Note |
|------------|--------|------|
| Get/set by dot path (nested) | ✅ | `get_actor_property`, `set_actor_property`, same for component |
| Get: Map, Set, Array, SoftObject, Object ref, structs | ✅ | `GetPropertyValueDirect` handles these |
| Set: Map, Set from JSON | ✅ | `SetPropertyValue` with JSON array of `{key,value}` / array of elements |
| Set: Array from JSON | ✅ | `SetPropertyValue` uses `FScriptArrayHelper` + `FJsonObjectConverter::JsonValueToUProperty` per element (primitives, structs, etc.) |
| Enumerate all properties of an object | ✅ | `get_all_properties` (object_path or actor + optional component_name) returns properties (name, type) |

---

## 9. Actors & spawning

| Capability | Status | Note |
|------------|--------|------|
| Spawn by class (with location, rotation, label, properties) | ✅ | `spawn_actor` |
| Delete / duplicate / transform / get bounds | ✅ | All use `ResolveActorFromParams` (world from params) |
| Place actor from asset (e.g. drag‑and‑drop equivalent) | ✅ | `place_actor_from_asset` (asset_path or object_path) places in current editor level |
| Spawn into specific level folder | ✅ | `spawn_actor` accepts optional `folder_path`; actor is placed in that outliner folder |
| Bulk delete by filter | ✅ | `delete_actors` (class_name, actor_label_contains, tags array); editor world only, uses DestroyActors |

---

## 9.5. AI (Behavior Tree, Blackboard, Perception, Navigation)

| Capability | Status | Note |
|------------|--------|------|
| Create Behavior Tree + Blackboard | ✅ | `create_behavior_tree` (asset_name, package_path) creates linked UBehaviorTree and UBlackboardData. |
| Add Blackboard keys | ✅ | `add_blackboard_key` (blackboard_path, key_name, key_type: Object, Vector, Float, Int, Bool, String). |
| Run Behavior Tree on AI Controller | ✅ | `run_behavior_tree` (controller_actor_path/label, behavior_tree_path/name; world params). |
| Configure AI Perception (Sight) | ✅ | `configure_ai_perception` (controller; sight_radius, lose_sight_radius, peripheral_vision_angle_degrees). |
| Configure AI Perception (Hearing) | ✅ | `configure_ai_hearing` (controller; hearing_range). Responds to ReportNoiseEvent (e.g. footsteps, gunshots). Requires perception component (add Sight first). |
| Rebuild NavMesh | ✅ | `rebuild_navigation` (optional world params) calls UNavigationSystemV1::Build(). |
| Add BT Composite (Selector / Sequence) | ✅ | `add_bt_composite_node` (behavior_tree_path/name, composite_type: "Selector" \| "Sequence", optional node_position). Editor-only; links new node under root. |
| Add BT Gate/MultiGate, Decorators, Services | ✅ | `add_blueprint_gate_node`, `add_blueprint_multigate_node` (Blueprint graph); `add_bt_decorator_node` (decorator_class, parent_node_guid), `add_bt_service_node` (service_class, parent_node_guid). |

---

## 10. Server & config

| Capability | Status | Note |
|------------|--------|------|
| Port, timeout, max commands/tick, max request size | ✅ | `[UnrealMCP]` in DefaultEngine.ini / GGameIni |
| Multi-client | ✅ | One thread per client, shared command queue |
| Ping / version | ✅ | `ping` returns status, version, engine, port |
| Get number of connected clients | ✅ | `get_status` returns `connected_clients`, `port`, `running` |
| Per-command or per-client rate limit | ✅ | Token bucket per client when `[UnrealMCP] RateLimitEnabled=1` (off by default) |

---

## 11. Not in scope (by design)

- **Animation / Sequencer / Niagara / Landscape / Physics assets:** No native commands; use `execute_python`.
- **Version control / lock / check-out:** Not implemented.
- **Structured error codes:** Errors are string messages only; no error code enum.
- **Reload asset after external change:** No `reload` command.

---

## 12. Quick reference: "No" list

| Missing | Workaround |
|--------|------------|
| AI: Damage / other perception senses | Sight and Hearing natively (`configure_ai_perception`, `configure_ai_hearing`); Damage/others via `execute_python` |
| Asset import (FBX/PNG) | `execute_python` + `unreal.AssetToolsHelpers.get_asset_tools().import_assets_automated(...)` |

---

## 13. Audit findings (refined limitations)

These are not full gaps but worth knowing when designing clients or scripts.

| Area | Limitation | Note |
|------|------------|------|
| **Blueprint resolution** | `blueprint_name` is matched by **short asset name** only | Commands use `FindBlueprintByName`, which matches `AssetName` (e.g. `BP_Foo`). Full path (e.g. `/Game/MyFolder/BP_Foo`) is not used; duplicate names in different folders can be ambiguous. |
| **create_asset** | Asset types that require a **source file** | Creating e.g. Texture from an on-disk file is not supported via params; use `execute_python` + AssetTools/import. Empty Texture or other factory-driven creates work where the factory supports it. |
| **Viewport** | **list_viewports** available | `list_viewports` returns all viewports with raw_index, perspective_index, is_perspective, fov, location, rotation. `get_viewport_transform` / `set_viewport_fov` accept optional `viewport_index`. |
| **get_status** | No per-client **token count** when rate limit on | When `RateLimitEnabled=1`, `get_status` returns `connected_clients`, `port`, `running` but not per-client remaining tokens. |
| **delete_actors** | Editor world only | Bulk delete does not run in PIE; world context params do not apply. |
| **Structured errors** | String messages only | No error code enum; clients must parse `error` string for debugging. |

---

## 14. State of the plugin (post–AI suite)

The native command set has grown from the original "remaining 8" roadmap to **100 command types**, with functional gaps eliminated. All capabilities in §§1–10 have native implementations. What remains are **behavioral constraints** (refined limitations) that clients and scripts should account for.

### Functional gaps: none

Gate and MultiGate Blueprint nodes are implemented natively: `add_blueprint_gate_node` and `add_blueprint_multigate_node` (see §3). No remaining features require an `execute_python` workaround for the capabilities listed in this audit.

### Refined limitations (the "fine print")

These are not missing features but **how commands behave under the hood**. Impact is for client/script design.

| Area | Refined limitation | Impact |
|------|--------------------|--------|
| Blueprint resolution | Matches by **short asset name** only (e.g. `BP_Foo`). | Two assets named `BP_Foo` in different folders may resolve ambiguously. |
| Asset creation | No **source file** support for imports (Textures, .fbx, etc.). | Empty assets via factories work; importing a .png or .fbx requires Python. |
| Viewports | **list_viewports** available. | `list_viewports` returns all viewports (index, fov, location, rotation). FOV and transform get/set accept `viewport_index`. |
| Bulk deletion | **Editor world only.** | `delete_actors` (with filters) does not run during PIE. |
| Server status | No **per-client token count**. | With rate limiting on, `get_status` does not report remaining tokens per client. |

### Major wins (home-stretch and earlier)

- **Property arrays:** `SetPropertyValue` uses `FScriptArrayHelper` and `FJsonObjectConverter` for JSON arrays natively.
- **Complex Blueprint nodes:** Native support for Timeline, ForEach, Switch (String), and Switch (Enum).
- **UMG tree:** Native `remove_umg_widget`, `set_umg_slot_content`, `get_umg_tree`.
- **Generic assets:** `create_asset` with factory discovery; DataTable via `row_struct`; rate limiting and `get_status`.
- **AI suite:** `create_behavior_tree`, `add_blackboard_key`, `run_behavior_tree`, `configure_ai_perception` (Sight), `configure_ai_hearing`, `add_bt_composite_node` (Selector/Sequence), `rebuild_navigation`; world/actor resolution shared with existing commands.

### Out of scope (by design)

The following remain in the realm of **execute_python** (or external tools):

- **Specialized assets:** Animation, Sequencer, Niagara, Landscape, Physics.
- **Workflow tools:** Version control (git/Perforce), asset reload after external change, structured error enums (errors stay string-based).

---

## Summary

The plugin covers **actors** (spawn with optional folder_path, delete, bulk delete_actors by filter, duplicate, transform, bounds, place from asset, folder, selection), **Blueprint** (graphs, create function/macro graph, event/call/branch/sequence/switch/variable nodes, connect, compile, node info, variables), **materials** (create, expressions, pins, connect, recompile), **UMG** (create widget blueprint, add/remove widget, tree, slot content), **assets** (list, metadata, save/delete/duplicate/rename, create_asset/blueprint/material/widget, create_content_folder), **AI** (create_behavior_tree, add_blackboard_key, run_behavior_tree, configure_ai_perception, configure_ai_hearing, add_bt_composite_node, rebuild_navigation), **level/world** (open, contexts, current level, loaded levels, load/unload streaming level in editor), **viewport** (selection, focus, transform, set FOV, screenshot), and **properties** (get/set including Map, Set, Array from JSON, enumerate all).

**Remaining limitations:** None for the capabilities in this audit. Gate/MultiGate, Hearing, and BT nodes are native. Behavioral constraints (short name resolution, editor-only bulk delete, no per-client token count in get_status, no source-file asset import) are documented in §§12–13. See **LIMITATIONS_AND_PYTHON_PATHS.md** for Python workarounds where needed (e.g. asset import, other senses).
