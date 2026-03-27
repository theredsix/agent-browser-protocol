# CDP Mode Design

## Overview

A new browser mode that suspends ABP control and starts Chrome's standard remote debugging server, allowing external CDP tools (captcha solvers, Puppeteer scripts, browser automation frameworks) to take full control of the browser. Extends the existing input mode system from two states (agent/human) to three (agent/human/cdp).

## State Model

The global `input_mode` field gains a third value:

| `input_mode` | System input | ABP input | Execution control | CDP server | Toolbar icon |
|---|---|---|---|---|---|
| `"agent"` (default) | Blocked | Active | Active (pause/resume) | Off | Robot (default) |
| `"human"` | Allowed | Blocked | Suspended | Off | Human (yellow) |
| `"cdp"` | Allowed | Blocked | Suspended | On (auto-port) | CDP plug (purple) |

The mode is **global** — applies to all tabs simultaneously.

### Transitions

```
Agent ──► Human (existing)
Agent ──► CDP
Human ──► CDP
CDP ──► Agent (only exit path)
```

**`agent` → `cdp`:**
1. Abort all in-flight ABP actions
2. Save execution state for all tabs (enabled, paused, virtual_time_base)
3. Resume execution on paused tabs (page runs freely)
4. Set `allow_system_inputs = true` on `RenderInputRouter` for all tabs
5. Detach ABP's `AbpCdpClient` from all tabs' `DevToolsAgentHost`
6. Detach ABP's `AbpCdpEventClient` via `AbpEventObserver::DetachTab()` for all tabs
7. Store list of tab IDs that had CDP clients for re-attach tracking
8. Start remote debugging server (auto-port selection, see below)
9. Start timeout timer if `timeout_ms` was provided
10. Set `input_mode_` to `kCdp`
11. Update toolbar icon to CDP plug (purple)

**`human` → `cdp`:**
1. Detach ABP's `AbpCdpClient` and `AbpCdpEventClient` from all tabs (steps 5-7 above)
2. Start remote debugging server (step 8 above)
3. Start timeout timer if provided (step 9 above)
4. Set `input_mode_` to `kCdp`
5. Update toolbar icon to CDP plug (purple)

Note: Execution state was already saved and suspended during the prior `agent` → `human` transition. System inputs are already allowed. No need to repeat.

**`cdp` → `agent`:**
1. Stop remote debugging server via `DevToolsAgentHost::StopRemoteDebuggingServer()` — tears down TCP socket, disconnects all external WebSocket clients
2. Cancel timeout timer if active
3. Iterate full `TabStripModel` to discover all tabs (including any created by the external CDP client)
4. For each tab: create `AbpCdpClient` via `GetOrCreateCdpClient()`, re-enable CDP domains (`Runtime.enable`, `Page.enable`, and if execution control was active: `Debugger.enable`, `Emulation.setVirtualTimePolicy`)
5. Re-register `AbpEventObserver` for all tabs
6. Clean up stale entries from `tab_states_` for tabs that were closed during CDP mode
7. Set `allow_system_inputs = false` on `RenderInputRouter` for all tabs
8. Restore saved execution state (re-pause tabs that were paused before handoff)
9. Set `input_mode_` to `kAgent`
10. Update toolbar icon to robot (default)

## Auto-Port Selection

When no explicit port is provided, ABP finds an available port automatically:

1. Start at port **24578**
2. Attempt to bind a TCP socket on `127.0.0.1:port`
3. If bind succeeds, close the socket and use that port for `StartRemoteDebuggingServer()`
4. If bind fails (EADDRINUSE), increment port and retry
5. Try up to **100 ports** (24578–24677)
6. If all 100 fail, return `503 Service Unavailable`

When an explicit `port` is provided in the request, try only that port. Fail with `503` if unavailable.

Note: There is a small race window between the pre-test close and the server binding the port. This is acceptable for localhost usage.

## API Design

### REST Endpoints

**`POST /api/v1/browser/cdp-mode/enter`**

Request body (all fields optional):
```json
{
  "port": 9222,
  "timeout_ms": 60000
}
```

Response `200`:
```json
{
  "status": "ok",
  "port": 24578,
  "ws_url": "ws://127.0.0.1:24578/devtools/browser/<guid>",
  "timeout_ms": 60000
}
```

The `ws_url` is constructed from `DevToolsAgentHost::GetRemoteDebuggingServerAddress()` (for host:port) and the browser target's `DevToolsAgentHost::GetId()` (for the GUID).

**`POST /api/v1/browser/cdp-mode/exit`**

No request body.

Response `200`:
```json
{
  "status": "ok"
}
```

### Error Responses

| Condition | Status | Body |
|---|---|---|
| Enter while already in CDP mode | `409` | `{"error": "already in cdp mode"}` |
| Exit while not in CDP mode | `409` | `{"error": "not in cdp mode"}` |
| Port unavailable (explicit or auto-walk exhausted) | `503` | `{"error": "no available port"}` |
| ABP action called while in CDP mode | `409` | `{"error": "browser is in cdp mode"}` |

### Browser Status Extension

`GET /api/v1/browser/status` includes CDP mode info when active:

```json
{
  "ready": true,
  "input_mode": "cdp",
  "cdp": {
    "port": 24578,
    "ws_url": "ws://127.0.0.1:24578/devtools/browser/<guid>",
    "remaining_ms": 34521
  }
}
```

When not in CDP mode, the `cdp` field is absent. `input_mode` is `"agent"` or `"human"`.

### MCP Tool

Single tool combining enter and exit:

```json
{
  "name": "cdp_mode",
  "description": "Enter or exit CDP mode for external browser control via Chrome DevTools Protocol",
  "inputSchema": {
    "type": "object",
    "properties": {
      "action": {
        "type": "string",
        "enum": ["enter", "exit"],
        "description": "Enter CDP mode to allow external CDP tools to control the browser, or exit to return control to ABP"
      },
      "port": {
        "type": "integer",
        "description": "CDP server port (default: auto-select starting at 24578)"
      },
      "timeout_ms": {
        "type": "integer",
        "description": "Auto-exit timeout in milliseconds (no timeout if omitted)"
      }
    },
    "required": ["action"]
  }
}
```

`port` and `timeout_ms` are ignored when `action` is `"exit"`.

### Blocked Operations in CDP Mode

Same blocking rules as human mode. All operations that create an `AbpActionContext` or modify browser state are blocked:

- click, type, scroll, move, drag, slider, clear_text
- keyboard/press, keyboard/down, keyboard/up
- execute (JS), wait
- screenshot (POST with action envelope)
- `GET /screenshot` with markup tags (requires CDP client for CSS injection)
- navigate, reload, back, forward
- dialog accept/dismiss
- permission grant/deny
- file chooser, select picker
- `POST /text` (requires CDP client for `Runtime.evaluate`)
- `POST /tabs` (create tab), `DELETE /tabs/{id}` (close tab) — external CDP client owns tab lifecycle
- input-mode changes (`POST /browser/input-mode`)

**Allowed (read-only):**
- `GET /screenshot` (binary, **without** markup tags — uses `GrabViewSnapshot` directly, no CDP client needed)
- `GET /tabs`, `GET /tabs/{id}`
- `GET /browser/status`
- `GET /browser/input-mode`
- `POST /browser/cdp-mode/exit`
- `GET /dialog`, `GET /downloads`, `GET /permissions`
- History endpoints
- Console endpoints

Blocked operations return:
```json
{"error": "Operation blocked: browser is in cdp mode"}
```

MCP blocked tools return error content:
```json
{
  "type": "text",
  "text": "Operation blocked: browser is in cdp mode. Use browser_get_status to check current input_mode, or use cdp_mode with action 'exit' to return control to ABP."
}
```

## Timeout & Safety

- `base::OneShotTimer` on `AbpController`, started after CDP server is confirmed up
- Timer uses **wall-clock time** (not virtual time, which is suspended during CDP mode)
- On expiry: identical sequence to explicit `cdp-mode/exit` (stop server, re-attach, restore state). External CDP clients receive an abrupt WebSocket close.
- Cancelled on explicit exit
- `remaining_ms` in browser status computed as `timeout_deadline - base::TimeTicks::Now()`

## Safety Guard: `GetOrCreateCdpClient`

`GetOrCreateCdpClient()` must refuse to create new CDP clients when `input_mode_ == kCdp`. Without this guard, any lazy code path (text extraction, markup screenshot, etc.) could silently attach a new CDP session that conflicts with the external tool's session. Returns nullptr in CDP mode; callers already handle nullptr from the existing tab-not-found case.

## Active Observers During CDP Mode

`WebContentsObserver`-based components remain active during CDP mode for continuity of read-only data collection:
- `AbpConsoleCapture` — continues capturing console messages
- `AbpDownloadObserver` — continues tracking downloads
- `AbpPermissionObserver` — continues intercepting permission prompts

These are harmless read-only observers that do not interfere with external CDP clients.

## Visual Indicators

### Toolbar Icon

The existing `AbpInputModeIconView` gains a third state:

| Mode | Icon | Color |
|---|---|---|
| Agent | Robot outline | Default/muted |
| Human | Human outline | Yellow |
| CDP | Plug/connection | Purple |

Clicking the icon while in CDP mode triggers `cdp-mode/exit` (same pattern as clicking in human mode triggers return to agent mode).

### Border Overlay

**Removed.** The compositor border overlay (yellow gradient for human mode) is removed for both human and CDP modes. The toolbar icon is the sole visual indicator. This simplifies mode transitions (no CSS injection/cleanup, no loss of border on navigation) and eliminates the dependency on CDP clients for visual indicators — which is essential since CDP clients are detached in CDP mode.

## CDP Client Detach/Re-attach Details

### Detach (on enter)

For each tab in `tab_states_`:
1. Reset `state.cdp_client` (unique_ptr reset calls destructor, which calls `host->DetachClient(this)`)
2. Call `event_observer_->DetachTab(tab_id)` to destroy the `AbpCdpEventClient`

### Re-attach (on exit)

Iterate `TabStripModel` (not saved tab list — tabs may have been created/closed):
1. For each `WebContents`, call `GetOrCreateCdpClient(wc)` — creates new `AbpCdpClient`, attaches to `DevToolsAgentHost`
2. Send CDP domain enable commands: `Runtime.enable`, `Page.enable`
3. If execution control was active for this tab (from saved state or new default): `Debugger.enable`, then `SendDeterministicPause()` if restoring paused state
4. Re-register with `AbpEventObserver` for history recording
5. For tabs created by the external CDP client (no prior ABP state): create full `TabState` with default configuration, apply same CDP domain setup

### Edge Cases

- **Tab closed during CDP mode:** Stale `tab_states_` entries are cleaned up by comparing against `TabStripModel` contents
- **Tab navigated during CDP mode:** CDP domain subscriptions are per-session, so re-attaching after navigation to a different origin may require re-enabling domains (handled by `GetOrCreateCdpClient` which always re-enables)
- **External CDP client still connected at timeout:** `StopRemoteDebuggingServer()` forcibly closes all WebSocket connections

## Implementation Files

| Component | File | Change |
|---|---|---|
| Mode state + transitions | `chrome/browser/abp/abp_controller.h/cc` | Add `kCdp` to `InputMode` enum, `EnterCdpMode()`, `ExitCdpMode()`, timer, port logic. Update `GetInputModeResponse` serialization (currently a ternary) to handle three values. |
| CDP-mode guard | `chrome/browser/abp/abp_controller.cc` | `GetOrCreateCdpClient` returns nullptr when `input_mode_ == kCdp` |
| Remote debugging server | `chrome/browser/abp/abp_controller.cc` | Call `DevToolsAgentHost::StartRemoteDebuggingServer()` / `StopRemoteDebuggingServer()` |
| Auto-port selection | `chrome/browser/abp/abp_controller.cc` | TCP bind test loop in `FindAvailableCdpPort()` |
| HTTP routing | `chrome/browser/abp/abp_http_server.cc` | Route `cdp-mode/enter`, `cdp-mode/exit` |
| Browser status | `chrome/browser/abp/abp_controller.cc` | Add `cdp` block to status response |
| MCP tool | `chrome/browser/abp/abp_mcp_handler.cc` | Register `cdp_mode` tool |
| MCP schema | `chrome/browser/abp/abp_tool_builder.cc` | Add `cdp_mode` tool schema |
| Toolbar icon | `chrome/browser/abp/abp_input_mode_icon_view.cc` | Add purple CDP icon state |
| Remove border overlay | `chrome/browser/abp/abp_input_mode_overlay.h/cc` | Delete files |
| Remove border overlay | `chrome/browser/abp/abp_controller.cc` | Remove border show/hide calls |
| Blocked operations | `chrome/browser/abp/abp_controller.cc` | Extend human-mode blocking check to include `kCdp` |
| BUILD.gn | `chrome/browser/abp/BUILD.gn` | Remove overlay files, no new files needed (logic in existing controller) |
