# UnrealMCP Plugin — Limitations & Python Paths

This document lists what is available natively and when to use **execute_python** instead. See **AUDIT_LIMITATIONS.md** for the full capability audit.

---

## Implemented in Native MCP

- **Interface graphs**: `FindGraphByName` searches `Blueprint->ImplementedInterfaces[].Graphs`; node ops search DelegateSignatureGraphs and ImplementedInterfaces.
- **Map/Set/Array properties**: `set_actor_property` / `set_component_property` accept Map (JSON array of `{key, value}`), Set (JSON array of elements), and Array from JSON.
- **World context**: Commands that need a world accept optional params **`use_pie`** (use first PIE world) and **`world_context_index`** (int). Used by: `get_all_actors`, `spawn_actor`, `duplicate_actor`, `console_command`, and others. Default is editor world.
- **Material**: **`create_material`**, **`get_material_expressions`**, **`add_material_expression`**, **`connect_material_expressions`**, **`connect_material_property`**, **`get_material_expression_pins`**, **`delete_material_expression`**, **`recompile_material`**.
- **UMG**: **`create_widget_blueprint`**, **`add_umg_widget`** (widget_class, parent_widget_name, widget_name), **`remove_umg_widget`**, **`set_umg_slot_content`**, **`get_umg_tree`** (full hierarchy).
- **Blueprint graph**: Event, CallFunction, Branch, Sequence, Switch (int/string/enum), Variable nodes; **Timeline**, **ForEach**, **Gate**, **MultiGate**; **`create_blueprint_graph`** (function/macro); connect, compile, set position, pin default, delete node, disconnect pins; **`get_node_info`** returns pins.
- **AI**: **`create_behavior_tree`**, **`add_blackboard_key`**, **`run_behavior_tree`**, **`configure_ai_perception`** (Sight), **`configure_ai_hearing`**, **`add_bt_composite_node`** (Selector/Sequence), **`add_bt_decorator_node`**, **`add_bt_service_node`**, **`rebuild_navigation`**.
- **Viewport**: **`list_viewports`** (all viewports with index, fov, location, rotation), **`get_viewport_transform`**, **`set_viewport_fov`**, **`focus_viewport`**, **`take_screenshot`**.
- **Assets**: **`get_assets`**, **`get_asset_full_metadata`**, **`get_selected_assets`**, **`set_selected_assets`**, **`create_asset`** (factory discovery; DataTable via `row_struct`), **`create_content_folder`**, **`get_content_subpaths`**; save, delete, duplicate, rename.
- **Outliner**: **`list_actor_folders`**, **`create_actor_folder`**, **`set_actor_folder`**, **`set_selected_actors_folder`**; **`get_all_actors`** returns `folder_path`.
- **Level/world**: **`get_world_contexts`**, **`get_current_level`**, **`get_loaded_levels`**, **`load_streaming_level`**, **`unload_streaming_level`** (editor); **`open_level`**.

---

## When to Use Python Instead

### 1. Asset import from file

Creating assets that require a **source file** (e.g. Texture from PNG, SkeletalMesh from FBX) is not supported via native params. Use **execute_python** with the Asset Tools API:

```python
import unreal
unreal.AssetToolsHelpers.get_asset_tools().import_assets_automated([...])
```

Empty Texture or other factory-driven creates work natively where the factory does not need a path.

### 2. Material / UMG (advanced or batch)

Native commands cover the common cases above. For advanced or batch edits, **execute_python** with `unreal.MaterialEditingLibrary` or `unreal.WidgetTree` remains an option.

---

### 3. AI: Damage and other perception senses

Sight and Hearing are native (**`configure_ai_perception`**, **`configure_ai_hearing`**). For Damage or other senses, use **execute_python** (e.g. `AISenseConfig_Damage`, custom senses).

### 4. Map/Set from Python callers

If you are **sending** Map/Set data **from Python** (e.g. building payloads for MCP), the engine expects **unreal.Map** / **unreal.Set** instances, not raw Python dict/set.

**Example:**

```python
my_map = unreal.Map(unreal.Name, unreal.Float)
my_map.add("Health", 100.0)
target_object.set_editor_property("MyMapProperty", my_map)
```

Native MCP **set_actor_property** / **set_component_property** now accept Map/Set **via JSON** in the request (see above), so when calling from an MCP client you can send a JSON array of `{key, value}` for maps or an array of elements for sets; no need to go through Python for that.

---

### 5. World context (PIE / multiple levels)

Native commands accept **`use_pie`** and **`world_context_index`** in params; default is editor world. For more complex world resolution, use **execute_python** with **unreal.EditorLevelLibrary.get_editor_world()** or **UnrealEditorSubsystem**.

### 6. Specialized assets (out of scope for native)

Animation, Sequencer, Niagara, Landscape, and Physics assets have no native MCP commands. Use **execute_python** or external tools. Version control (lock/check-out) and asset reload after external change are also not implemented.

---

## Summary

| Feature                         | Native MCP                                                    | Python fallback                    |
|---------------------------------|---------------------------------------------------------------|------------------------------------|
| Interface graphs                | ✅ FindGraphByName, node ops in ImplementedInterfaces         | —                                  |
| Map/Set/Array property **set**   | ✅ JSON in set_*_property                                     | unreal.Map / unreal.Set (from Py)  |
| Material (create, connect, pins) | ✅ create_material, connect_material_*, add/delete expression, recompile | MaterialEditingLibrary (advanced) |
| UMG (tree, slot, create)        | ✅ add/remove_umg_widget, set_umg_slot_content, get_umg_tree, create_widget_blueprint | WidgetTree APIs (advanced)         |
| Blueprint (Gate, Timeline, etc.) | ✅ add_blueprint_gate_node, add_blueprint_multigate_node, Timeline, ForEach, Switch string/enum | —                                  |
| AI (BT, Blackboard, Sight/Hearing)| ✅ create_behavior_tree, add_blackboard_key, run_behavior_tree, configure_ai_perception, configure_ai_hearing, add_bt_*_node, rebuild_navigation | Damage/other senses                |
| Viewport (list, transform, FOV) | ✅ list_viewports, get_viewport_transform, set_viewport_fov, focus_viewport, take_screenshot | —                                  |
| Asset import from file          | ❌                                                            | AssetTools.import_assets_automated |
| World context (PIE/multi)        | ✅ use_pie, world_context_index in params                     | EditorLevelLibrary (complex)       |
