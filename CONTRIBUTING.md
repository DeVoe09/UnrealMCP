# Contributing to UnrealMCP

Thank you for your interest in contributing! This document provides guidelines for contributors.

## How to Contribute

### Reporting Bugs

Open a GitHub issue using the **Bug Report** template. Include steps to reproduce, expected vs. actual behavior, your Unreal Engine version, and OS.

### Suggesting Features

Use the **Feature Request** issue template to describe the problem and your proposed solution.

### Submitting Pull Requests

1. Fork the repository and create a feature branch from `main`.
2. Make your changes following the code style guidelines below.
3. Test your changes in the Unreal Editor (compile and verify affected commands).
4. Fill out the pull request template.
5. Submit the PR and wait for review.

## Code Style

This project follows [Epic's Coding Standard](https://dev.epicgames.com/documentation/en-us/unreal-engine/epic-cplusplus-coding-standard-for-unreal-engine):

- **Prefix classes** with `F` (structs), `U` (UObject), `A` (AActor), `S` (Slate), `E` (enums).
- **Command handlers** are named `Cmd_<CommandName>` and take `TSharedPtr<FMCPPendingCommand>&`.
- **Error handling**: Always call `SetError(Cmd, ...)` with a descriptive message and error code. Never silently ignore failures.
- **Null safety**: Check pointers before use. Use `IsValid()` for UObjects, null checks for raw pointers.
- **JSON access**: Always use `TryGetStringField`, `TryGetObjectField`, `TryGetNumberField`, etc. (safe). Never use `GetStringField` / `GetObjectField` without checking — they can assert on missing keys.

## Adding a New Command

1. Declare the handler in `MCPTCPServer.h`.
2. Implement it in `MCPTCPServer.cpp` following existing patterns.
3. Register it in `DispatchCommand()`.
4. Document it in `README.md`.
5. Add or extend a test in `Scripts/`: AI-related commands are covered by `Scripts/test_ai_commands.py`; for other domains, add a manual test or a small script (see `Scripts/README.md`).

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).
