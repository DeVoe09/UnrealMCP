# Changelog

All notable changes to UnrealMCP will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.1] - 2026-03-14

### Fixed

- **set_actor_transform**: Replaced `GetObjectField` with `TryGetObjectField` for `location`, `rotation`, and `scale`. Omitting these params no longer triggers an Unreal Engine assert.
- **JSON-RPC `id` handling**: In `ParseCommand`, the JSON-RPC 2.0 `id` field is now read via `TryGetField` and only treated as number or string; `null` or other types no longer risk unsafe access.

### Added

- **docs/DEEP_AUDIT_REPORT.md**: Deep audit report covering industry standards (threading, errors, config, security), identified bugs, and optional hardening (path validation, input size limits).

### Changed

- **README.md**: Example ping response version in "Verify Installation" updated to match the plugin version (2.0.1).

---

## [2.0.0] - 2026-03-14

### Added

#### Transport & Protocol (Section 1)
- **Bind address config**: New `BindAddress` option in `[UnrealMCP]` config. Set to `127.0.0.1` or `localhost` to bind to loopback only (safer on shared networks). Default remains `0.0.0.0`.
- **Health endpoint**: New `health` command returns server status, version, engine info, and connection count — useful for monitoring and "is the editor ready?" checks.

#### Domain Commands (Section 2)
- **Asset import**: `import_asset` — import files from disk (FBX, PNG, etc.) via `IAssetTools::ImportAssets`.
- **Asset reload**: `reload_asset` — reimport an asset after external changes.
- **Lighting**: `create_light` (directional/point/spot/rect), `edit_light` (intensity, color, mobility), `build_lighting` (trigger light build).
- **Landscape**: `create_landscape` (terrain creation), `get_landscape_info` (query bounds, components, resolution).
- **Foliage**: `place_foliage`, `query_foliage` (list types and instance counts), `remove_foliage` (by type).
- **Sequencer**: `create_level_sequence` (with optional world placement), `add_sequencer_track`, `play_sequence` (play/pause/stop/scrub).
- **Niagara/VFX**: `create_niagara_system` (spawn particle systems in level), `set_particle_parameter`.
- **Audio**: `add_audio_component` — attach audio to actors with sound, volume, and auto-activate settings.
- **World Partition**: `get_world_partition_info`, `get_data_layers` — query data layers and partition state.
- **Physics**: `create_physics_constraint` — create ball, hinge, prismatic, or fixed constraints between actors.
- **AI extensions**: `configure_ai_damage_perception`, `set_blackboard_value_runtime` (set values in PIE), `possess_pawn`.

#### Server Behavior & QoL (Section 3)
- **Per-client token count in `get_status`**: When rate limiting is enabled, response now includes `client_tokens` object with per-client remaining tokens and `rate_limit_enabled` flag.
- **Enhanced `get_status`**: Now also reports `bind_address`.
- **Batch execution**: `batch_execute` — run multiple commands in one request with optional undo transaction wrapping.
- **Undo/Redo**: `begin_transaction`, `end_transaction`, `undo`, `redo` — full editor undo/redo support with named transactions.

#### MCP & Discovery (Section 4)
- **Richer `inputSchema`**: New `MakeRichSchema` helper adds `description` fields to all new tool parameter schemas, giving LLMs and clients better hints.
- **MCP Prompts**: `prompts/list` endpoint exposing workflow prompts (Blueprint best practices, material workflow, level design workflow, MCP command guide).
- **MCP Resources**: `resources/list` endpoint exposing project info, common asset paths, and common class names.
- **Engine version in `initialize`**: Server info now includes `engine_version` alongside plugin version.
- **Capabilities in `initialize`**: Now advertises `prompts` and `resources` capabilities per MCP spec.

### Changed
- Version bumped from 1.1.0 to 2.0.0 (major version: 30 new commands, new MCP capabilities).
- Plugin description updated to reflect expanded feature set.
- Build dependencies expanded: Landscape, LandscapeEditor, Foliage, LevelSequence, LevelSequenceEditor, MovieScene, MovieSceneTracks, Niagara, NiagaraEditor, AudioMixer, PhysicsCore, HTTP, AnimGraph, AnimGraphRuntime.

---

## [1.1.0] - 2026-03-14

### Added

- MIT License for open-source distribution.
- `.gitignore` for clean repository tracking (excludes Binaries, Intermediate, IDE files).
- `CONTRIBUTING.md` with code style guidelines, PR process, and new command guide.
- `SECURITY.md` with responsible disclosure policy and security considerations.
- `CHANGELOG.md` following Keep a Changelog format.
- GitHub issue templates for bug reports and feature requests.
- GitHub pull request template.
- Version constant `UNREALMCP_VERSION` for consistent version reporting.

### Fixed

- Null pointer dereference in `FClientConnectionRunnable::Run()` when server is destroyed during client disconnect.
- Assertion crash in `Cmd_SpawnActor` when `location` or `rotation` params are missing (switched from `GetObjectField` to safe `TryGetObjectField`).
- Potential null world pointer passed to `GEngine->Exec()` in `Cmd_ConsoleCommand`.
- Buffer overflow vulnerability in `ReadLine()` where buffer could grow beyond `MaxRequestLineBytes` limit.

### Changed

- Version bumped from 1.0.0 to 1.1.0.
- Improved error messages with more descriptive context.

## [1.0.0] - 2026-01-01

### Added

- Initial release of UnrealMCP plugin for Unreal Engine 5.7.
- TCP server on configurable port (default 55557) with newline-delimited JSON protocol.
- 100+ native command handlers across actors, blueprints, materials, UMG, and AI.
- Multi-client support with per-client threads and MPSC command queue.
- Optional token-bucket rate limiting.
- Toolbar status widget with connection state indicator (red/amber/green with pulse animation).
- Python script execution via PythonScriptPlugin integration.
- Behavior Tree, Blackboard, AI Perception, and NavMesh commands.
- Blueprint graph editing: events, functions, branches, sequences, switches, timelines, loops.
- Material expression editing and connection management.
- UMG widget tree manipulation.
- Configurable via `DefaultEngine.ini` (port, timeout, max commands per tick, rate limiting).
