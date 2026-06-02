# Agent Browser Protocol - Chromium Fork

A Chromium fork implementing the Agent Browser Protocol (ABP) - a REST-based API for AI agent browser control at the C++ engine level. Unlike CDP or browser extensions, ABP operates directly in the browser engine for lower latency and greater capability.

## Current Implementation Status

### Working Features
- **Tab Management**: List, create, close, get info, activate, stop loading
- **Navigation**: Navigate to URL, back, forward, reload
- **Screenshots**: Capture viewport with optional element markup overlays (GET binary or POST with action envelope)
- **Mouse Input**: Click, move, scroll (native mouse wheel events via RenderWidgetHost)
- **Keyboard Input**: Type text, press/down/up key events with modifier support
- **JavaScript Execution**: Execute scripts and get results (via CDP Runtime.evaluate)
- **Text Extraction**: Get page text or text from a CSS selector
- **Dialogs**: Get pending dialog info, accept, dismiss (alert/confirm/prompt/beforeunload)
- **Downloads**: List, get status, cancel
- **File Chooser**: Provide files to native file picker dialogs
- **Native Pickers**: Intercept `<select>` dropdowns (cross-platform, incl. Linux/Windows via `use_external_popup_menu`) and date/time pickers (`date`/`time`/`datetime-local`/`month`/`week`, via a `ShowDateTimePopup` mojo → `AbpDateTimeChooser`), surfacing a `select_open`/`datetime_picker_open` event; respond via `/select/{id}` and `/datetime-picker/{id}` (ISO values, applied by Blink)
- **Permissions**: Intercept permission prompts, grant/deny with type validation (geolocation grant accepts lat/lng/accuracy, auto-deny non-geolocation)
- **Execution Control**: Pause/resume JS execution with virtual time for deterministic state
- **Wait**: Duration-based wait with action envelope
- **History**: Session, action, and event history with SQLite storage
- **Input Mode**: Toggle between agent (ABP-controlled), human (user-controlled), and cdp (remote debugging) input modes via API or toolbar icon. Human mode allows direct user interaction (e.g., for authentication), suspends execution control, and shows a yellow gradient border overlay.
- **Browser Management**: Status check, graceful shutdown
- **Console Capture**: In-memory 5000-entry FIFO buffer capturing console.log/warn/error, CORS errors, CSP violations, uncaught exceptions via WebContentsObserver (non-fingerprintable, no CDP)
- **MCP Server**: Embedded MCP (JSON-RPC over HTTP) with 21 tools at `/mcp`

### Architecture

```
┌─────────────────────────────────────────────┐
│         HTTP Client (curl/agent/MCP)        │
└─────────────────┬───────────────────────────┘
                  │ GET/POST /api/v1/* or /mcp
                  ▼
┌─────────────────────────────────────────────┐
│  AbpHttpServer (IO thread)                  │
│  - net::HttpServer on localhost:8222        │
│  - Routes REST + MCP requests               │
└─────────────────┬───────────────────────────┘
                  │ PostTask to UI thread
                  ▼
┌─────────────────────────────────────────────┐
│  AbpController (UI thread)                  │
│  - Direct access to Browser, TabStripModel  │
│  - Uses DevToolsAgentHost for CDP commands  │
│  - AbpActionContext for action lifecycle    │
│  - AbpInputDispatcher for native input      │
│  - AbpEventObserver for CDP event streams   │
│  - AbpEventCollector for action events      │
├─────────────────────────────────────────────┤
│  AbpMcpHandler - Embedded MCP server        │
│  AbpHistoryController - Session/action log  │
│  AbpDownloadObserver - Download tracking    │
└─────────────────────────────────────────────┘
```

## Project Structure

### ABP Source Code

```
chrome/browser/abp/
├── BUILD.gn                     # Build configuration
├── abp_switches.h/cc            # --abp-port, --abp-session-dir flags
├── abp_http_server.h/cc         # HTTP server (IO thread)
├── abp_controller.h/cc          # Request handler + CDP client (UI thread)
├── abp_action_context.h/cc      # Action lifecycle (pause/resume/screenshot)
├── abp_input_dispatcher.h/cc    # Native input dispatch (click/scroll/keys)
├── abp_location_provider.h/cc   # Mock geolocation provider (coordinates set via permission grant)
├── abp_system_geolocation_source.h/cc # Bypasses macOS system location dialog
├── abp_permission_observer.h/cc # Permission prompt interception
├── abp_event_observer.h/cc      # CDP event client per tab
├── abp_event_collector.h/cc     # Collects events during actions
├── abp_mcp_handler.h/cc         # Embedded MCP server (JSON-RPC over HTTP)
├── abp_tool_builder.h/cc        # MCP tool schema builder
├── abp_history_controller.h/cc  # Session/action history API
├── abp_history_database.h/cc    # SQLite history storage
├── abp_download_observer.h/cc   # Download tracking
├── abp_console_capture.h/cc     # Console message capture (WebContentsObserver)
├── abp_config.h/cc              # Runtime configuration
└── abp_types.h                  # Shared type definitions
```

### Design Documentation

```
plans/
├── README.md                    # Overview
├── agent-browser-protocol.md    # Core ABP architecture
├── API.md                       # Full REST API specification
├── mcp.md                       # MCP server specification
└── implementation.md            # Minimal implementation plan
```

## Build Setup

### Prerequisites

1. **depot_tools** (Google's build toolchain):
   ```bash
   git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git ~/depot_tools
   echo 'export PATH="$HOME/depot_tools:$PATH"' >> ~/.bashrc
   source ~/.bashrc
   ```

2. **Build dependencies** (Ubuntu/Debian):
   ```bash
   sudo ./build/install-build-deps.sh --no-prompt
   ```

### Directory Structure

```
/home/paladin/src/
├── .gclient           # gclient configuration
├── src -> chromium    # symlink required by gclient
└── chromium/          # source code
    ├── out/Default/   # build output
    ├── chrome/browser/abp/  # ABP implementation
    ├── tools/abp-mcp-server/  # MCP server
    └── plans/         # Design docs
```

### Sync Dependencies

```bash
cd /home/paladin/src
gclient sync --no-history
```

### Configure Build

Debug component build (faster incremental builds):
```bash
cd /home/paladin/src/src
gn gen out/Default --args='is_debug=true is_component_build=true symbol_level=1 dcheck_always_on=true'
```

### Build Chromium

```bash
cd /home/paladin/src/src
autoninja -C out/Default chrome
```

First build: ~4-6 hours. Incremental builds: seconds to minutes.

## Running ABP

### Start ABP

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S)
```

Session data (database and screenshots) will be stored in `sessions/<timestamp>/`.

To use the default `/tmp/abp-<UUID>/` directory instead:
```bash
./out/Default/ABP.app/Contents/MacOS/ABP
```

### REST API Examples

```bash
# Check browser readiness
curl http://localhost:8222/api/v1/browser/status

# List all tabs
curl http://localhost:8222/api/v1/tabs

# Create new tab
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Navigate existing tab
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Take screenshot with element markup
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/screenshot \
  -H "Content-Type: application/json" \
  -d '{"screenshot":{"markup":"interactive","format":"webp"}}'

# Binary screenshot (returns image/webp)
curl http://localhost:8222/api/v1/tabs/{tab_id}/screenshot?markup=interactive -o screenshot.webp

# Click at coordinates
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/click \
  -H "Content-Type: application/json" \
  -d '{"x":100,"y":200}'

# Type text
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/type \
  -H "Content-Type: application/json" \
  -d '{"text":"hello world"}'

# Press key combo (Ctrl+A)
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/keyboard/press \
  -H "Content-Type: application/json" \
  -d '{"key":"a","modifiers":["Control"]}'

# Scroll down at coordinates
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/scroll \
  -H "Content-Type: application/json" \
  -d '{"x":500,"y":400,"delta_y":300}'

# Execute JavaScript (note: parameter is "script", not "expression")
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/execute \
  -H "Content-Type: application/json" \
  -d '{"script":"document.title"}'

# Get page text
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/text \
  -H "Content-Type: application/json" \
  -d '{}'

# Close tab
curl -X DELETE http://localhost:8222/api/v1/tabs/{tab_id}
```

### MCP Server

The MCP server is embedded directly in Chrome—no separate process needed. It's available at `/mcp` on the same port as the REST API.

Configure in Claude Desktop (`claude_desktop_config.json`):
```json
{
  "mcpServers": {
    "browser": {
      "transport": "streamable-http",
      "url": "http://localhost:8222/mcp"
    }
  }
}
```

Test the MCP endpoint:
```bash
# Initialize MCP session
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}'

# List available tools
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
```

## API Reference

See `plans/API.md` for the complete REST API specification. All endpoints:

| Method | Path | Description |
|--------|------|-------------|
| **Browser** | | |
| GET | `/api/v1/browser/status` | Get browser readiness status |
| GET | `/api/v1/browser/session-data` | Get session data file paths |
| POST | `/api/v1/browser/shutdown` | Graceful shutdown |
| GET | `/api/v1/browser/input-mode` | Get current input mode (agent/human) |
| POST | `/api/v1/browser/input-mode` | Set input mode (toggles human/agent control) |
| POST | `/api/v1/browser/cdp-mode/enter` | Enter CDP mode (starts remote debugging server) |
| POST | `/api/v1/browser/cdp-mode/exit` | Exit CDP mode (returns control to ABP) |
| **Tabs** | | |
| GET | `/api/v1/tabs` | List all tabs |
| GET | `/api/v1/tabs/{id}` | Get tab details |
| POST | `/api/v1/tabs` | Create new tab |
| DELETE | `/api/v1/tabs/{id}` | Close tab |
| POST | `/api/v1/tabs/{id}/activate` | Switch to tab |
| POST | `/api/v1/tabs/{id}/stop` | Stop loading |
| **Navigation** | | |
| POST | `/api/v1/tabs/{id}/navigate` | Navigate to URL |
| POST | `/api/v1/tabs/{id}/reload` | Reload page |
| POST | `/api/v1/tabs/{id}/back` | Go back |
| POST | `/api/v1/tabs/{id}/forward` | Go forward |
| **Mouse** | | |
| POST | `/api/v1/tabs/{id}/click` | Click at coordinates |
| POST | `/api/v1/tabs/{id}/move` | Mouse move |
| POST | `/api/v1/tabs/{id}/scroll` | Scroll (mouse wheel) |
| **Keyboard** | | |
| POST | `/api/v1/tabs/{id}/type` | Type text |
| POST | `/api/v1/tabs/{id}/keyboard/press` | Press key combo |
| POST | `/api/v1/tabs/{id}/keyboard/down` | Key down |
| POST | `/api/v1/tabs/{id}/keyboard/up` | Key up |
| **Screenshots** | | |
| GET | `/api/v1/tabs/{id}/screenshot` | Binary WebP screenshot |
| POST | `/api/v1/tabs/{id}/screenshot` | Screenshot via action envelope |
| **Content** | | |
| POST | `/api/v1/tabs/{id}/execute` | Execute JavaScript |
| POST | `/api/v1/tabs/{id}/text` | Get page text |
| **Wait** | | |
| POST | `/api/v1/tabs/{id}/wait` | Wait for duration |
| **Dialogs** | | |
| GET | `/api/v1/tabs/{id}/dialog` | Get pending dialog |
| POST | `/api/v1/tabs/{id}/dialog/accept` | Accept dialog |
| POST | `/api/v1/tabs/{id}/dialog/dismiss` | Dismiss dialog |
| **Execution Control** | | |
| GET | `/api/v1/tabs/{id}/execution` | Get execution state |
| POST | `/api/v1/tabs/{id}/execution` | Set execution state |
| **Downloads** | | |
| GET | `/api/v1/downloads` | List downloads |
| GET | `/api/v1/downloads/{id}` | Get download status |
| POST | `/api/v1/downloads/{id}/cancel` | Cancel download |
| **File Chooser** | | |
| POST | `/api/v1/file-chooser/{id}` | Provide files to dialog |
| **Popups** | | |
| POST | `/api/v1/select/{id}` | Respond to select popup |
| POST | `/api/v1/datetime-picker/{id}` | Respond to date/time picker (ISO value or cancel) |
| **Permissions** | | |
| GET | `/api/v1/permissions` | List pending permission requests |
| POST | `/api/v1/permissions/{id}/grant` | Grant permission (requires permission_type; geolocation requires lat/lng) |
| POST | `/api/v1/permissions/{id}/deny` | Deny permission (requires permission_type) |
| **Console** | | |
| GET | `/api/v1/console` | Query console messages (level, pattern, tab_id, limit, after_id) |
| DELETE | `/api/v1/console` | Clear console buffer (optional tab_id filter) |
| **Network** | | |
| GET | `/api/v1/network` | Query saved network calls with regex filters |
| POST | `/api/v1/network/save` | Retroactively tag & persist in-memory buffer |
| DELETE | `/api/v1/network` | Clear saved calls (by tag or all) |
| POST | `/api/v1/tabs/{id}/curl` | Execute HTTP request using tab's session |
| **History** | | |
| GET | `/api/v1/history/sessions` | List sessions |
| GET | `/api/v1/history/sessions/current` | Get current session |
| GET | `/api/v1/history/sessions/{id}` | Get session by ID |
| GET | `/api/v1/history/sessions/{id}/export` | Export session |
| GET | `/api/v1/history/actions` | List actions |
| GET | `/api/v1/history/actions/{id}` | Get action by ID |
| GET | `/api/v1/history/actions/{id}/screenshot` | Get action screenshot |
| DELETE | `/api/v1/history/actions` | Delete actions |
| GET | `/api/v1/history/events` | List events |
| GET | `/api/v1/history/events/{id}` | Get event by ID |
| DELETE | `/api/v1/history/events` | Delete events |
| DELETE | `/api/v1/history` | Delete all history |
| **MCP** | | |
| POST | `/mcp` | MCP JSON-RPC endpoint (21 tools, includes `cdp_mode`) |

## Development Notes

- Source is at `/home/paladin/src/chromium` (symlinked as `src` for gclient)
- Build output at `out/Default/`
- Use `autoninja` (not `ninja`) for automatic parallelism
- Run `gclient sync` after pulling changes to update dependencies
- ABP uses CDP (Chrome DevTools Protocol) internally for JS evaluation and debugger control
- Mouse/keyboard input is dispatched natively via `RenderWidgetHost` (bypasses CDP)
- Screenshots use `ForceRedraw` + `GrabViewSnapshot` (not CDP `Page.captureScreenshot`)
- Tab IDs are DevToolsAgentHost IDs (stable for the session)
- Execution control uses `Debugger.pause/resume` + `Emulation.setVirtualTimePolicy` for deterministic state
- Session history is stored in SQLite via `AbpHistoryDatabase`
