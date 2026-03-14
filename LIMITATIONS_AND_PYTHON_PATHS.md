# UnrealMCP Plugin — Limitations & Python Paths

This document lists what is available natively and when to use **execute_python** instead.

---

## Implemented in Native MCP

- **Interface graphs**: `FindGraphByName` searches `Blueprint->ImplementedInterfaces[].Graphs`.
- **Map/Set properties**: `set_actor_property` / `set_component_property` accept Map (JSON array of `{key, value}`) and Set (JSON array of elements).
- **World context**: Commands that need a world accept optional params **`use_pie`** (use first PIE world) and **`world_context_index`** (int). Used by: `get_all_actors`, `spawn_actor`, `duplicate_actor`, `console_command`. Default is editor world.
- **Material connections**: **`connect_material_expressions`** (from/to expression by guid or name, optional pin names), **`connect_material_property`** (from expression to material property e.g. BaseColor), **`add_material_expression`** (expression_class, optional node_position), **`recompile_material`**.
- **UMG**: **`add_umg_widget`** — widget_blueprint_path, widget_class (e.g. Button, TextBlock), optional parent_widget_name, optional widget_name. Adds the widget to the tree under root or the given panel parent.

---

## When to Use Python Instead

### 1. Material / UMG

Native commands cover the common cases above. For advanced or batch edits, **execute_python** with `unreal.MaterialEditingLibrary` or `unreal.WidgetTree` remains an option.

---

### 2. Map/Set from Python callers

If you are **sending** Map/Set data **from Python** (e.g. building payloads for MCP), the engine expects **unreal.Map** / **unreal.Set** instances, not raw Python dict/set.

**Example:**

```python
my_map = unreal.Map(unreal.Name, unreal.Float)
my_map.add("Health", 100.0)
target_object.set_editor_property("MyMapProperty", my_map)
```

Native MCP **set_actor_property** / **set_component_property** now accept Map/Set **via JSON** in the request (see above), so when calling from an MCP client you can send a JSON array of `{key, value}` for maps or an array of elements for sets; no need to go through Python for that.

---

### 3. World context (PIE / multiple levels)

Native commands accept **`use_pie`** and **`world_context_index`** in params; default is editor world. For more complex world resolution, use **execute_python** with **unreal.EditorLevelLibrary.get_editor_world()** or **UnrealEditorSubsystem**.

---

## Summary

| Feature                     | Native MCP                                               | Python fallback              |
|----------------------------|----------------------------------------------------------|------------------------------|
| Interface graphs           | ✅ FindGraphByName                                       | —                            |
| Map/Set property **set**   | ✅ JSON in set_*_property                                | unreal.Map / unreal.Set      |
| Material **connections**  | ✅ connect_material_*, add_material_expression, recompile | MaterialEditingLibrary       |
| UMG widget **tree**        | ✅ add_umg_widget                                        | WidgetTree APIs              |
| World context (PIE/multi)  | ✅ use_pie, world_context_index in params                | EditorLevelLibrary           |
