<p align="center">
  <img src="https://img.shields.io/badge/Unreal%20Engine-5.7+-blue?style=for-the-badge&logo=unrealengine" alt="UE 5.7+"/>
  <img src="https://img.shields.io/badge/MCP-Compatible-green?style=for-the-badge" alt="MCP Compatible"/>
  <img src="https://img.shields.io/badge/License-MIT-yellow?style=for-the-badge" alt="MIT License"/>
  <img src="https://img.shields.io/badge/Commands-100+-purple?style=for-the-badge" alt="100+ Commands"/>
  <img src="https://img.shields.io/badge/Platform-Windows-lightgrey?style=for-the-badge&logo=windows" alt="Windows"/>
</p>

# UnrealMCP

**A Model Context Protocol (MCP) server plugin for Unreal Engine** that lets AI assistants control the editor programmatically. Exposes 100+ native commands over TCP with JSON-RPC 2.0 support — actors, blueprints, materials, UMG widgets, AI behavior trees, and more.

Drop it into your project's `Plugins/` folder, enable it, and connect any MCP-compatible client (Claude Code, custom scripts, etc.) to start building with AI.

---

## Features

- **100+ native commands** covering actors, blueprints, materials, UMG, AI, assets, viewport, and more
- **MCP protocol support** with JSON-RPC 2.0 (`initialize`, `tools/list`, `tools/call`) for standard MCP client compatibility
- **Legacy protocol** backward-compatible with simple `{"type":"command","params":{...}}` JSON format
- **Multi-client support** with dedicated threads per connection and MPSC command queue
- **Game-thread execution** ensuring all UE API calls happen safely on the correct thread
- **Toolbar status widget** with real-time connection indicator (red/amber/green with pulse animation)
- **Python integration** via `execute_python` for anything not covered by native commands
- **Rate limiting** with configurable token-bucket per client
- **Full property system** with dot-path navigation, type-aware JSON conversion, Map/Set/Array support

---

## Installation

### From Source (Plugin Folder)

1. Clone or download this repository into your project's `Plugins/` directory:

```
YourProject/
  Plugins/
    UnrealMCP/
      Source/
      UnrealMCP.uplugin
      ...
```

2. Regenerate project files (right-click your `.uproject` > "Generate Visual Studio project files").
3. Build and launch the Unreal Editor.
4. The plugin auto-starts a TCP server on port `55557`. Look for the **MCP** indicator in the toolbar.

### Verify Installation

Connect with any TCP client and send:

```json
{"id":"1","type":"ping","params":{}}
```

You should receive:

```json
{"id":"1","success":true,"result":{"status":"pong","version":"2.0.0","engine":"5.7.0","port":55557}}
```

---

## Quick Start

### Legacy Protocol (Simple JSON)

```json
{"id":"1","type":"spawn_actor","params":{"class":"StaticMeshActor","label":"MyCube","location":{"x":0,"y":0,"z":100}}}
```

### MCP Protocol (JSON-RPC 2.0)

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-05","clientInfo":{"name":"my-client","version":"1.0"}}}
```

```json
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
```

```json
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"spawn_actor","arguments":{"class":"PointLight","label":"MyLight","location":{"x":100,"y":0,"z":200}}}}
```

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  MCP Client (Claude Code, Python script, etc.)               │
│  Sends: {"jsonrpc":"2.0","method":"tools/call",...}          │
└───────────────────────┬──────────────────────────────────────┘
                        │ TCP port 55557
                        ▼
┌──────────────────────────────────────────────────────────────┐
│  FMCPTCPServer                                               │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ Accept Thread (FRunnable::Run)                         │  │
│  │ • Listens for incoming TCP connections                 │  │
│  │ • Spawns per-client FClientConnectionRunnable          │  │
│  └────────────────────────────────────────────────────────┘  │
│                        │                                     │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ Per-Client Threads                                     │  │
│  │ • Reads newline-delimited JSON from socket             │  │
│  │ • Parses command (legacy or JSON-RPC 2.0)              │  │
│  │ • Enqueues to MPSC command queue                       │  │
│  │ • Waits on completion event (configurable timeout)     │  │
│  └────────────────────────────────────────────────────────┘  │
│                        │                                     │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ Game Thread (FTickableEditorObject::Tick)               │  │
│  │ • Dequeues commands (up to MaxCommandsPerTick)         │  │
│  │ • Dispatches to Cmd_* handler                          │  │
│  │ • Signals completion event → unblocks client thread    │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────┘
```

---

## Command Reference

All commands accept `actor_path` or `actor_label` for actor resolution, and optional `use_pie` / `world_context_index` for world selection.

### Core

| Command | Key Params | Description |
|---------|-----------|-------------|
| `ping` | — | Connectivity check; returns version and engine info |
| `get_status` | — | Connected clients, port, running state |
| `execute_python` | `code` | Run Python in editor (requires PythonScriptPlugin) |
| `execute_python_file` | `file_path` | Run a Python script file |
| `console_command` | `command` | Execute a UE console command |
| `get_project_info` | — | Project name, paths, engine version |

### MCP Protocol

| Command | Description |
|---------|-------------|
| `initialize` | JSON-RPC 2.0 handshake; returns server info and capabilities |
| `tools/list` | Returns all tool definitions with inputSchema for LLM discovery |
| `tools/call` | Unified tool invocation: `{"name":"<tool>","arguments":{...}}` |

### Level & World

| Command | Key Params | Description |
|---------|-----------|-------------|
| `open_level` | `level_path` | Load a level |
| `get_world_contexts` | — | List world contexts |
| `get_current_level` | — | Current level info |
| `get_loaded_levels` | — | All loaded + streaming levels |
| `load_streaming_level` | `level_path` | Add streaming level |
| `unload_streaming_level` | `level_path` | Remove streaming level |

### Actors

| Command | Key Params | Description |
|---------|-----------|-------------|
| `get_all_actors` | `class_filter`, `label_filter` | List actors with optional filters |
| `spawn_actor` | `class`, `label`, `location`, `rotation` | Spawn actor |
| `delete_actor` | actor params | Delete one actor |
| `delete_actors` | `class_name`, `tags` | Bulk delete by filter |
| `duplicate_actor` | actor params | Clone actor |
| `get_actor_transform` | actor params | Get location/rotation/scale |
| `set_actor_transform` | actor + transform | Set transform |
| `get_actor_bounds` | actor params | Get origin and extent |
| `get_actor_property` | actor + `property` | Read property by dot-path |
| `set_actor_property` | actor + `property`, `value` | Set property (JSON) |
| `get_all_properties` | actor or `object_path` | List all properties |
| `get_actor_components` | actor params | List components |
| `place_actor_from_asset` | `asset_path` | Place from content asset |
| `set_component_property` | actor + `component`, `property`, `value` | Set component property |
| `get_component_property` | actor + `component`, `property` | Get component property |

### Outliner & Folders

| Command | Key Params | Description |
|---------|-----------|-------------|
| `set_actor_folder` | actor + `folder_path` | Move actor to folder |
| `set_selected_actors_folder` | `folder_path` | Move selection to folder |
| `list_actor_folders` | — | List all folders |
| `create_actor_folder` | `folder_path` | Create folder |

### Assets & Content Browser

| Command | Key Params | Description |
|---------|-----------|-------------|
| `get_assets` | `path`, `class_filter` | List assets |
| `get_selected_assets` | — | Content Browser selection |
| `set_selected_assets` | `asset_paths` | Set CB selection |
| `create_asset` | `asset_name`, `package_path`, `asset_class` | Create asset |
| `create_blueprint` | `blueprint_name`, `package_path`, `parent_class` | Create Blueprint |
| `create_material` | `asset_path` | Create material |
| `save_asset` / `save_level` / `save_all` | — | Save operations |
| `delete_asset` / `duplicate_asset` / `rename_asset` | `asset_path` | Asset management |
| `get_asset_full_metadata` | `asset_path` | Full metadata + tags |
| `get_unsaved_assets` | — | List dirty assets |
| `get_content_subpaths` | `base_path` | Folder hierarchy |
| `create_content_folder` | `folder_path` | Create content folder |

### Blueprint Graph

| Command | Key Params | Description |
|---------|-----------|-------------|
| `get_blueprint_graphs` | `blueprint_name` | List graphs |
| `create_blueprint_graph` | `blueprint_name`, `graph_name`, `graph_type` | Create function/macro graph |
| `find_blueprint_nodes` | `blueprint_name` | Find nodes |
| `get_node_info` | `blueprint_name`, `node_id` | Node details |
| `add_blueprint_event_node` | `blueprint_name`, `event_name` | Add event |
| `add_blueprint_function_node` | `blueprint_name`, `function_name` | Add function call |
| `add_blueprint_branch_node` | `blueprint_name` | Add Branch |
| `add_blueprint_sequence_node` | `blueprint_name` | Add Sequence |
| `add_blueprint_switch_node` | `blueprint_name` | Add Switch (int) |
| `add_blueprint_switch_string_node` | `blueprint_name` | Add Switch (string) |
| `add_blueprint_switch_enum_node` | `blueprint_name`, `enum_path` | Add Switch (enum) |
| `add_blueprint_timeline_node` | `blueprint_name` | Add Timeline |
| `add_blueprint_foreach_node` | `blueprint_name` | Add ForEach |
| `add_blueprint_gate_node` | `blueprint_name` | Add Gate |
| `add_blueprint_multigate_node` | `blueprint_name` | Add MultiGate |
| `connect_blueprint_nodes` | `source_node_id`, `source_pin`, `target_node_id`, `target_pin` | Connect pins |
| `disconnect_blueprint_pins` | same as connect | Break link |
| `set_node_position` | `node_id`, `x`, `y` | Move node |
| `set_pin_default_value` | `node_id`, `pin_name`, `value` | Set pin default |
| `delete_blueprint_node` | `node_id` | Delete node |
| `compile_blueprint` | `blueprint_name` | Compile |

### Blueprint Variables

| Command | Key Params | Description |
|---------|-----------|-------------|
| `get_blueprint_variables` | `blueprint_name` | List variables |
| `add_blueprint_variable` | `blueprint_name`, `variable_name`, `variable_type` | Add variable |
| `set_blueprint_variable_default` | `blueprint_name`, `variable_name`, `value` | Set default |
| `add_blueprint_variable_node` | `blueprint_name`, `variable_name`, get/set | Add get/set node |

### Materials

| Command | Key Params | Description |
|---------|-----------|-------------|
| `get_material_expressions` | `material_path` | List expressions |
| `add_material_expression` | `material_path`, `expression_class` | Add expression |
| `delete_material_expression` | `material_path` | Remove expression |
| `connect_material_expressions` | `material_path`, `from`, `to` | Connect expressions |
| `connect_material_property` | `material_path`, `from`, `property` | Connect to property |
| `get_material_expression_pins` | `material_path` | Get pin info |
| `recompile_material` | `material_path` | Recompile |

### UMG (Widget Blueprint)

| Command | Key Params | Description |
|---------|-----------|-------------|
| `create_widget_blueprint` | `asset_name`, `package_path` | Create Widget BP |
| `add_umg_widget` | `widget_blueprint_path`, `widget_class` | Add widget |
| `remove_umg_widget` | `widget_blueprint_path`, `widget_name` | Remove widget |
| `get_umg_tree` | `widget_blueprint_path` | Widget hierarchy |
| `set_umg_slot_content` | `widget_blueprint_path`, `slot_name` | Set named slot |

### AI (Behavior Tree, Blackboard, Perception)

| Command | Key Params | Description |
|---------|-----------|-------------|
| `create_behavior_tree` | `asset_name`, `package_path` | Create BT + Blackboard pair |
| `add_blackboard_key` | `blackboard_path`, `key_name`, `key_type` | Add Blackboard key |
| `run_behavior_tree` | controller + `behavior_tree_path` | Run BT on AI Controller |
| `configure_ai_perception` | controller params | Add/configure Sight sense |
| `configure_ai_hearing` | controller params | Add/configure Hearing sense |
| `add_bt_composite_node` | `behavior_tree_path`, `composite_type` | Add Selector/Sequence |
| `add_bt_decorator_node` | `behavior_tree_path` | Add decorator |
| `add_bt_service_node` | `behavior_tree_path` | Add service |
| `rebuild_navigation` | — | Rebuild NavMesh |

### Viewport & Selection

| Command | Key Params | Description |
|---------|-----------|-------------|
| `get_selected_actors` | — | List selected actors |
| `set_selected_actors` | `actor_paths` | Set selection |
| `focus_viewport` | `actor_label` or coordinates | Move camera |
| `get_viewport_transform` | — | Camera position/rotation/FOV |
| `set_viewport_fov` | `fov` | Set FOV |
| `take_screenshot` | `filename` | Request screenshot |
| `list_viewports` | — | Enumerate viewports |

---

## Configuration

Add to your project's `DefaultEngine.ini`:

```ini
[UnrealMCP]
Port=55557
CommandTimeoutSeconds=30
MaxCommandsPerTick=16
MaxRequestLineBytes=16777216
RateLimitEnabled=0
```

| Setting | Default | Description |
|---------|---------|-------------|
| `Port` | 55557 | TCP listen port |
| `CommandTimeoutSeconds` | 30 | Max seconds to wait for game-thread execution |
| `MaxCommandsPerTick` | 16 | Max commands processed per editor tick |
| `MaxRequestLineBytes` | 16 MB | Max single request size |
| `RateLimitEnabled` | 0 | Enable per-client token-bucket rate limiting |

---

## Project Structure

```
UnrealMCP/
├── Source/UnrealMCP/
│   ├── Public/
│   │   ├── MCPTCPServer.h          # Server class, command queue, protocol
│   │   ├── MCPStatusWidget.h       # Toolbar indicator widget
│   │   └── UnrealMCPModule.h       # Module interface
│   ├── Private/
│   │   ├── MCPTCPServer.cpp        # Server + all 100+ command handlers
│   │   ├── MCPStatusWidget.cpp     # Widget implementation
│   │   └── UnrealMCPModule.cpp     # Module startup, config, toolbar
│   └── UnrealMCP.Build.cs          # Build rules and dependencies
├── Scripts/
│   ├── test_ai_commands.py         # Python test suite
│   └── README.md                   # Test documentation
├── docs/
│   └── AI_COMMANDS_IMPLEMENTATION_PLAN.md
├── UnrealMCP.uplugin               # Plugin manifest
├── LICENSE                          # MIT License
├── CONTRIBUTING.md                  # Contribution guidelines
├── CHANGELOG.md                     # Version history
├── SECURITY.md                      # Security policy
└── README.md                        # This file
```

---

## Requirements

- **Unreal Engine** 5.7+ (Editor builds only)
- **Platform**: Windows (Win64)
- **Optional**: PythonScriptPlugin (for `execute_python` commands)

---

## Security

This plugin opens a TCP server on your machine for development use. See [SECURITY.md](SECURITY.md) for details on network exposure and responsible disclosure.

---

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on reporting bugs, submitting PRs, code style, and adding new commands.

---

## License

This project is licensed under the [MIT License](LICENSE).

---

## See Also

- [Model Context Protocol Specification](https://modelcontextprotocol.io/)
- [AUDIT_LIMITATIONS.md](AUDIT_LIMITATIONS.md) — Known limitations and workarounds
- [LIMITATIONS_AND_PYTHON_PATHS.md](LIMITATIONS_AND_PYTHON_PATHS.md) — When to use Python fallback
