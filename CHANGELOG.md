# Changelog

All notable changes to UnrealMCP will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
