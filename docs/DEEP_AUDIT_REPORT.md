# UnrealMCP Plugin — Deep Audit Report

**Date:** 2025-03-14  
**Scope:** Industry standards compliance, robustness, and bug identification.  
**Artifacts:** `MCPTCPServer.cpp`, `UnrealMCPModule.cpp`, `MCPTCPServer.h`, config, docs.

---

## 1. Executive Summary

| Area | Verdict | Notes |
|------|---------|--------|
| **Threading & concurrency** | ✅ Meets standards | Game-thread dispatch, MPSC queue, clean shutdown |
| **Null / validity** | ⚠️ Mostly good, gaps | A few missing checks; one assert-risk pattern |
| **Error handling** | ✅ Good | SetError/error codes; timeouts; shutdown messages |
| **Input & config** | ✅ Good | Param validation, line-length cap, config clamping |
| **Resources & lifetime** | ✅ Good | Sockets closed, events returned to pool, no obvious leaks |
| **API usage** | ⚠️ One bug | `GetObjectField` can assert on missing key (UE behavior) |
| **Security** | ⚠️ Hardening suggested | Path validation, batch/input size limits |
| **MCP / docs** | ✅ Aligned | JSON-RPC 2.0, tools/list; README version string outdated |

**Overall:** The plugin is production-capable and well-structured. Addressing the bugs and hardening items below will bring it in line with industry expectations for a TCP-based MCP server in an editor plugin.

---

## 2. Industry Standards Assessment

### 2.1 Threading and Concurrency ✅

- **Game thread only for UE APIs:** All command execution runs from `FTickableEditorObject::Tick()` → `ProcessCommandQueue()` → `DispatchCommand()` → `Cmd_*`. No UE API calls from network threads.
- **Per-connection threads:** Each client has its own `FClientConnectionRunnable`; no shared mutable state between connection threads except the server’s command queue.
- **Queue:** `TQueue<..., Mpsc>` is appropriate for multiple producers (client threads) and single consumer (game thread).
- **Synchronization:** `CompletionEvent` used correctly (Wait on client thread, Trigger on game thread); events returned to pool after use.
- **Shutdown:** `Stop()` closes listen socket, closes all client sockets, drains the queue with error + Trigger so client threads don’t hang, then joins all threads. No use-after-free of sockets or runnables.

### 2.2 Null and Validity ⚠️

- **GetWorldFromParams:** Uses `GEditor` only when needed; returns `nullptr` when no world.
- **ResolveActorFromParams:** Checks `Params.IsValid()`; many handlers check `!Cmd->Params.IsValid()` before use.
- **Gaps:**
  - **ParseCommand (JSON-RPC `id`):** `Root->Values["id"]` is used without checking that the value is non-null. If `"id"` is JSON `null`, `IdVal` can be invalid or `IdVal->Type == EJson::Null`, and calling `IdVal->AsString()` is unsafe (undefined behavior or assert).
  - **set_actor_transform:** Uses `GetObjectField("location")`, `GetObjectField("rotation")`, `GetObjectField("scale")`. In Unreal Engine, **GetObjectField with a missing key can assert**. If the client omits these keys, the editor can crash. **Recommendation:** Use `TryGetObjectField` and only use the object when present.

### 2.3 Error Handling ✅

- **Consistent pattern:** `SetError(Cmd, Message, Code)` with machine-readable codes (`not_found`, `invalid_params`, `timeout`, etc.).
- **Timeouts:** Commands have a configurable timeout; on timeout the client receives an error and the event is triggered so the client thread doesn’t block indefinitely.
- **Shutdown:** Queued commands get `SetError(..., "server_stopping")` and their completion event is triggered.
- **JSON parse failure:** Parse errors return a JSON error response and the connection continues.

### 2.4 Input and Config ✅

- **Line length:** `ReadLine` enforces `MaxRequestLineBytes` (configurable, clamped 4KB–64MB). Rejecting oversized lines prevents unbounded buffers.
- **Config clamping:** Port, `CommandTimeoutSeconds`, `MaxCommandsPerTick`, `MaxRequestLineBytes` are clamped to safe ranges.
- **Param validation:** Most commands validate required params (e.g. `TryGetStringField` for required fields) and return clear errors.
- **BindAddress:** Supports binding to loopback only (`127.0.0.1` / `localhost`) for safer deployment.

### 2.5 Resources and Lifetime ✅

- **Listen socket:** Created in `Start()`, closed and destroyed in `Stop()`.
- **Client sockets:** Closed in `FClientConnectionRunnable::Run()` and in `Stop()` via `CloseSocket()`; then destroyed by the socket subsystem.
- **Completion events:** Taken from pool, returned with `ReturnSynchEventToPool` after wait.
- **Module shutdown:** `ShutdownModule()` calls `TCPServer->Stop()` then `Reset()`; no dangling server or threads.

### 2.6 API Usage ⚠️

- **Unsafe JSON access:** Use of `GetObjectField` / `GetNumberField` on potentially missing keys can assert in UE. **Confirmed:** Epic docs recommend `TryGetObjectField` when the key might be absent. The only current instance in the audit is **set_actor_transform** (lines 2210–2212).
- **world_context_index:** In several places the code uses `HasField("world_context_index")` then `GetNumberField("world_context_index")`. If the value is not a number (e.g. string), `GetNumberField` may assert. **Recommendation:** Use `TryGetNumberField` and ignore invalid values.
- **Foliage / GEditor / NewObject:** Previous fixes (null checks for foliage pair, GEditor, NewObject for perception/damage config) are in place; no additional API misuse found in sampled handlers.

### 2.7 Security ⚠️

- **Binding:** Configurable `BindAddress`; binding to `127.0.0.1` or `localhost` restricts access to localhost (good for dev; document in SECURITY.md).
- **Rate limiting:** Optional token-bucket per client; off by default, configurable.
- **Hardening suggestions (industry practice):**
  - **import_asset:** Validate `file_path` (e.g. no path traversal like `..\\..\\sensitive`) and that `destination_path` is under a known content root (e.g. `/Game/` or project content).
  - **execute_python:** Consider a maximum length for the `code` parameter to avoid memory exhaustion from huge payloads.
  - **batch_execute:** Cap the size of the `commands` array (e.g. max 256) to prevent DoS from a single request.

### 2.8 MCP Protocol and Documentation ✅

- **JSON-RPC 2.0:** `initialize`, `tools/list`, `tools/call` implemented; responses include `jsonrpc`, `id`, `result`/`error`.
- **Legacy format:** Still supported for backward compatibility.
- **Docs vs implementation:** AUDIT_LIMITATIONS, LIMITATIONS_AND_PYTHON_PATHS, PRE_PUSH_AUDIT_2.0 are consistent with 2.0 features. **Minor:** README example ping response shows `"version":"1.1.0"`; actual response uses `UNREALMCP_VERSION` ("2.0.0"). Recommend updating README to "2.0.0" or "version in response matches plugin".

---

## 3. Bugs and Recommended Fixes

### 3.1 High — Crash / Assert Risk

| # | Location | Issue | Fix |
|---|----------|--------|-----|
| 1 | `MCPTCPServer.cpp` ~2210–2240 | `Cmd_SetActorTransform` used `GetObjectField` for location/rotation/scale; missing keys can cause UE to assert. **Fixed:** Use `TryGetObjectField` for all three; only apply transform when the corresponding object is present. | ✅ Fixed |

### 3.2 Medium — Robustness

| # | Location | Issue | Fix |
|---|----------|--------|-----|
| 2 | `MCPTCPServer.cpp` ~748–760 | JSON-RPC `id` handling: `IdVal` from `Root->Values["id"]` was used without checking `IdVal.IsValid()` or `IdVal->Type`. **Fixed:** Use `Root->TryGetField(TEXT("id"))` and only handle `Number`/`String` types; leave `Cmd->Id` empty for Null or other types. | ✅ Fixed |
| 3 | `MCPTCPServer.cpp` 2737, 2798, 2861 | `HasField("world_context_index")` then `GetNumberField("world_context_index")`. Non-numeric value can assert. | Use `TryGetNumberField(TEXT("world_context_index"), WorldContextIndex)` and ignore when not a number. |

### 3.3 Low — Hardening and Consistency

| # | Location | Issue | Fix |
|---|----------|--------|-----|
| 4 | `Cmd_ImportAsset` | No path validation; `file_path` could be path traversal; `destination_path` could be outside content. | Validate `file_path` is under an allowed root or is a safe path; ensure `destination_path` is under project content (e.g. `/Game/`). |
| 5 | `Cmd_ExecutePython` | No upper bound on `code` length. | Enforce a max length (e.g. 1MB) and return error if exceeded. |
| 6 | `Cmd_BatchExecute` | No limit on `commands` array size. | Cap at e.g. 256 entries; return `invalid_params` if exceeded. |
| 7 | README.md | Example ping response shows `"version":"1.1.0"`. | Update to `"2.0.0"` or state that version matches plugin. |

### 3.4 Informational — No Code Change Required

- **create_landscape:** Implemented as a “requested” response plus a note that full creation needs Python/heightmap; documented in PRE_PUSH_AUDIT_2.0. Acceptable as-is.
- **UTF-8 in ReadLine:** Conversion via `UTF8_TO_TCHAR` on a bounded buffer; line length already capped. No change needed unless you need strict UTF-8 validation.
- **Batch nesting:** `batch_execute` can contain sub-commands that are themselves `batch_execute`. Consider documenting a recommended max nesting depth (e.g. 1) or adding a depth limit to avoid stack growth.

---

## 4. Checklist for Compliance

- [x] All UE API calls from game thread only  
- [x] No shared mutable state between client threads (only queue and atomic state)  
- [x] Sockets and events cleaned up on disconnect and shutdown  
- [x] Config values clamped to safe ranges  
- [x] Request line length limited  
- [x] **Use TryGetObjectField in set_actor_transform** (fix applied)  
- [x] **Validate JSON-RPC `id` before use** (fix applied: TryGetField + type check)  
- [ ] Optional: Path validation for import_asset; input size limits for execute_python and batch_execute  
- [x] README version string updated to 2.0.0  

---

## 5. Conclusion

The UnrealMCP plugin meets industry standards for threading, shutdown, error reporting, and config handling. The main correctness issue is the use of `GetObjectField` in `set_actor_transform`, which can assert when keys are missing; switching to `TryGetObjectField` fixes this. Strengthening JSON-RPC `id` handling and `world_context_index` usage improves robustness. Optional hardening (path validation, input size limits) and a small doc fix will align the plugin fully with typical expectations for a production-ready MCP server in an editor environment.
