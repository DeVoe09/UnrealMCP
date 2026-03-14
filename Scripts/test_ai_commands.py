#!/usr/bin/env python3
"""
Test script for UnrealMCP AI commands.

Prerequisites:
- Unreal Editor open with the Emo project and UnrealMCP plugin loaded.
- MCP server running (default port 55557).

Usage:
  python test_ai_commands.py [--port 55557] [--level /Game/AncestralPlane/Lvl_Hub]

Creates a new level (or opens the given one), then runs:
  ping, get_status, create_content_folder, create_behavior_tree, add_blackboard_key,
  add_bt_composite_node (Selector + Sequence), open_level, spawn AIController,
  configure_ai_perception, configure_ai_hearing, run_behavior_tree, rebuild_navigation.
"""

import argparse
import json
import socket
import sys
import uuid

DEFAULT_PORT = 55557
DEFAULT_LEVEL = "/Game/AncestralPlane/Lvl_Hub"
TEST_PACKAGE = "/Game/MCPTest"
TEST_BT_NAME = "MCPTestBT"
CONTROLLER_LABEL = "MCPTestAIController"


def send_command(sock: socket.socket, cmd_type: str, params: dict | None = None, debug: bool = False) -> dict:
    req = {"id": str(uuid.uuid4()), "type": cmd_type, "params": params or {}}
    msg = (json.dumps(req) + "\n").encode("utf-8")
    sock.sendall(msg)
    buf = b""
    while b"\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("Connection closed")
        buf += chunk
    line = buf.split(b"\n", 1)[0].decode("utf-8", errors="replace").strip("\r")
    if debug:
        print(f"[DEBUG] raw response length={len(line)}: {line[:250]!r}")
    try:
        return json.loads(line)
    except json.JSONDecodeError as e:
        if len(line) < 20:
            raise ValueError(
                f"Server sent a very short response ({len(line)} chars): {line!r}. "
                "Ensure Unreal Editor is open with the Emo project and UnrealMCP plugin (TCP on port 55557)."
            ) from e
        raise ValueError(f"Invalid JSON (raw first 300 chars): {line[:300]!r}") from e


def main() -> int:
    parser = argparse.ArgumentParser(description="Test UnrealMCP AI commands")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="MCP TCP port")
    parser.add_argument("--level", type=str, default=DEFAULT_LEVEL, help="Level path to open (if not creating new)")
    parser.add_argument("--no-new-level", action="store_true", help="Skip creating new level; only open --level")
    parser.add_argument("--debug", action="store_true", help="Print raw first response for debugging")
    args = parser.parse_args()
    debug = args.debug

    results = []

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(60)
        sock.connect(("127.0.0.1", args.port))
    except OSError as e:
        print(f"Failed to connect to 127.0.0.1:{args.port}: {e}")
        print("Ensure Unreal Editor is open with the Emo project and MCP server is running.")
        return 1

    def run(name: str, cmd: str, params: dict | None = None) -> bool:
        try:
            r = send_command(sock, cmd, params, debug=debug)
            ok = r.get("success", False)
            err = r.get("error", "")
            res = r.get("result", r)
            results.append((name, ok, err, res))
            if ok:
                print(f"  OK  {name}")
                return True
            print(f"  FAIL {name}: {err}")
            return False
        except Exception as e:
            results.append((name, False, str(e), None))
            print(f"  ERR {name}: {e}")
            return False

    print("=== UnrealMCP AI commands test ===\n")

    # 1) Connectivity
    if not run("ping", "ping"):
        sock.close()
        return 1
    run("get_status", "get_status")

    # 2) New level or open level
    if not args.no_new_level:
        # Try to create a new level via Python (optional; may not exist in all UE versions)
        run("execute_python (new level)", "execute_python", {
            "code": "import unreal; unreal.EditorLevelLibrary.new_level()"
        })
        # If that failed, we'll open_level next; if it succeeded we're already in a new level
    run("open_level", "open_level", {"level_path": args.level})

    # 3) Content folder
    run("create_content_folder", "create_content_folder", {"folder_path": TEST_PACKAGE})

    # 4) Behavior Tree + Blackboard
    run("create_behavior_tree", "create_behavior_tree", {
        "asset_name": TEST_BT_NAME,
        "package_path": TEST_PACKAGE
    })
    # Get paths from last result for next steps
    bt_path = blackboard_path = None
    for _name, _ok, _err, res in reversed(results):
        if isinstance(res, dict):
            bt_path = res.get("behavior_tree_path") or bt_path
            blackboard_path = res.get("blackboard_path") or blackboard_path
        if bt_path and blackboard_path:
            break

    if blackboard_path:
        run("add_blackboard_key", "add_blackboard_key", {
            "blackboard_path": blackboard_path,
            "key_name": "TargetActor",
            "key_type": "Object"
        })
        run("add_blackboard_key (Float)", "add_blackboard_key", {
            "blackboard_path": blackboard_path,
            "key_name": "WaitTime",
            "key_type": "Float"
        })

    if bt_path:
        run("add_bt_composite_node (Selector)", "add_bt_composite_node", {
            "behavior_tree_path": bt_path,
            "composite_type": "Selector",
            "node_position": [200, 0]
        })
        run("add_bt_composite_node (Sequence)", "add_bt_composite_node", {
            "behavior_tree_path": bt_path,
            "composite_type": "Sequence",
            "node_position": [400, 100]
        })

    # 5) Spawn AIController in editor world
    run("spawn_actor (AIController)", "spawn_actor", {
        "class": "AIController",
        "label": CONTROLLER_LABEL,
        "location": {"x": 0, "y": 0, "z": 100}
    })

    # 6) Perception (Sight then Hearing)
    run("configure_ai_perception", "configure_ai_perception", {
        "controller_actor_label": CONTROLLER_LABEL,
        "sight_radius": 2000,
        "lose_sight_radius": 2500,
        "peripheral_vision_angle_degrees": 90
    })
    run("configure_ai_hearing", "configure_ai_hearing", {
        "controller_actor_label": CONTROLLER_LABEL,
        "hearing_range": 1500
    })

    # 7) Run Behavior Tree (may fail in editor if BT expects a Pawn; that's expected)
    if bt_path:
        run("run_behavior_tree", "run_behavior_tree", {
            "controller_actor_label": CONTROLLER_LABEL,
            "behavior_tree_path": bt_path
        })

    # 8) Rebuild navigation
    run("rebuild_navigation", "rebuild_navigation", {})

    sock.close()

    # Summary
    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    print(f"\n=== Result: {passed}/{total} passed ===")
    if passed < total:
        print("Failed steps:")
        for name, ok, err, _ in results:
            if not ok:
                print(f"  - {name}: {err}")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
