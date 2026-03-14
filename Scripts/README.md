# UnrealMCP Scripts

## test_ai_commands.py

End-to-end test for the AI-related MCP commands. Run with **Unreal Editor open** (Emo project, UnrealMCP plugin loaded, MCP server listening on port 55557).

### Usage

```bash
# From repo root or plugin folder (Python 3.7+)
python Plugins/UnrealMCPPlugin/Scripts/test_ai_commands.py
# If you cloned the repo as UnrealMCP: python Plugins/UnrealMCP/Scripts/test_ai_commands.py
```

Options:

- `--port 55557` — MCP TCP port (default: 55557).
- `--level <path>` — Level to open (default: `UNREALMCP_TEST_LEVEL` env var, or `/Game/AncestralPlane/Lvl_Hub`). Used after an optional “new level” attempt; if you only want to open this level, use `--no-new-level`.
- `--no-new-level` — Do not try to create a new level via `execute_python`; only open the level given by `--level`.

### What it tests

1. **ping** / **get_status** — Connectivity.
2. **New level** (optional) — `execute_python` with `unreal.EditorLevelLibrary.new_level()`; may fail depending on UE version.
3. **open_level** — Opens the specified level (default: project’s Lvl_Hub).
4. **create_content_folder** — Creates `/Game/MCPTest`.
5. **create_behavior_tree** — Creates `MCPTestBT_BT` and `MCPTestBT_BB` in `/Game/MCPTest`.
6. **add_blackboard_key** — Adds `TargetActor` (Object) and `WaitTime` (Float) to the blackboard.
7. **add_bt_composite_node** — Adds a Selector and a Sequence to the behavior tree graph.
8. **spawn_actor** — Spawns an `AIController` with label `MCPTestAIController`.
9. **configure_ai_perception** — Configures sight on that controller.
10. **configure_ai_hearing** — Configures hearing on that controller.
11. **run_behavior_tree** — Runs the created BT on the controller (may fail in editor if no Pawn is possessed).
12. **rebuild_navigation** — Rebuilds NavMesh for the current world.

### Expected outcomes

- **ping**, **get_status**, **create_content_folder**, **create_behavior_tree**, **add_blackboard_key**, **add_bt_composite_node**, **spawn_actor**, **configure_ai_perception**, **configure_ai_hearing**, **rebuild_navigation** should succeed when the editor and level are valid.
- **execute_python (new level)** may fail on some UE versions (API difference).
- **run_behavior_tree** may fail in editor if the controller has no Pawn (expected when testing without PIE).

Exit code: 0 if all steps passed, 1 otherwise.

### Troubleshooting

- **Connection refused:** Unreal Editor not running, or UnrealMCP plugin not loaded. Open the Emo project in the editor; the TCP server starts automatically on port 55557.
- **Invalid JSON / very short response:** Another process may be using port 55557, or the connection is not to UnrealMCP. Run with `--debug` to see the raw response. On Windows, check: `netstat -an | findstr 55557`.
- **run_behavior_tree fails:** Expected in editor without PIE if the controller has no Pawn. Run PIE and then use `use_pie: true` in params to test runtime.
