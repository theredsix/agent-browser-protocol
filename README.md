# Agent Browser Protocol

<img width="384" height="256" alt="ChatGPT Image Jan 25, 2026, 03_59_19 PM" src="https://github.com/user-attachments/assets/6cf0b584-b708-4c75-a146-dd49750e92f0" /> 


**Browsers are async. Agents are synchronous. ABP turns continuous browsing into discrete, atomic steps—so LLMs can reason about the web without racing against it.**

A Chromium fork with a REST + MCP API built directly into the browser engine. One request = one completed step (settled state + screenshot + event log).

```
    AI Agent                                 ABP Chromium
        │                                         │
        │  POST /click (x=450, y=320)             │
        │────────────────────────────────────────>│
        │                                         │  Inject real input event
        │                                         │  Wait for page to settle
        │                                         │  Capture compositor screenshot
        │                                         │  Collect events (e.g. tab_created)
        │                                         │  ┌─────────────────────────────┐
        │                                         │  │ PAUSE JavaScript + virtual  │
        │                                         │  │ time                        │
        │                                         │  └─────────────────────────────┘
        │  200 OK: screenshot + events            │
        │<────────────────────────────────────────│
        │                                         │
        ·  (agent inspects screenshot, decides)   ·
        │                                         │
        │  POST /type (text="Show HN")            │
        │────────────────────────────────────────>│
        │                                         │  ┌─────────────────────────────┐
        │                                         │  │ UNPAUSE JavaScript + virtual│
        │                                         │  │ time                        │
        │                                         │  └─────────────────────────────┘
        │                                         │  Inject real keyboard events
        │                                         │  Wait for page to settle
        │                                         │  Capture compositor screenshot
        │                                         │  Collect events
        │                                         │  ┌─────────────────────────────┐
        │                                         │  │ PAUSE JavaScript + virtual  │
        │                                         │  │ time                        │
        │                                         │  └─────────────────────────────┘
        │  200 OK: screenshot + events            │
        │<────────────────────────────────────────│
        │                                         │
```

No WebSocket. No CDP session management. No Puppeteer abstraction layers.
Just `curl http://localhost:8222/api/v1/tabs` and you're in.

---

## Why Fork Chromium?

Web browsing is inherently asynchronous—events fire unpredictably, pages settle on their own timeline, state changes continuously. LLMs reason synchronously—one observation, one decision, one action. This mismatch is fundamental: existing tools force agents to race against a live browser, guessing when actions complete and papering over timing with retries.

Extensions can't fix this (sandboxed). CDP can't fix this (designed for DevTools, not autonomous control). Playwright and Puppeteer inherit the same model. We needed to go deeper.

**ABP reformats browsing into a step machine**: a request/response contract where the agent only ever acts on a stable, frozen world state.

| What agents need | What existing tools provide |
|------------------|----------------------------|
| Pause JavaScript between actions | Debugging pause (breaks the page) |
| Pause time between actions | Real-time only |
| Compositor-layer cursor rendering | No cursor visibility |
| Simple REST API | WebSocket + session management |
| Engine-level event injection | DOM simulation or CDP passthrough |
| Action-complete detection | Manual waits or flaky heuristics |
| Event list between actions (new tab, dialog, file picker, etc.) | Polling, or async event subscriptions |

**Each API call is one atomic step.** ABP injects real input through Chromium's input system, waits for an engine-defined "settled" boundary, captures compositor output (with cursor), and returns the events that occurred. JavaScript and virtual time freeze between steps. The agent never races against the browser—it observes, decides, acts, and repeats on a world that waits for it.

---

## Quick Start

### Pre-built Binaries

Coming soon. For now, build from source.

### Run ABP Chromium

```bash
./chrome --enable-abp
```

The REST API starts on `localhost:8222`. That's it.

### Your First API Call

```bash
# List open tabs
curl http://localhost:8222/api/v1/tabs

# Create a new tab and navigate
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url": "https://news.ycombinator.com"}'

# Click the first link (with element markup in response screenshot)
curl -X POST http://localhost:8222/api/v1/tabs/{TAB_ID}/click \
  -H "Content-Type: application/json" \
  -d '{"x": 450, "y": 320, "screenshot": {"markup": "interactive"}}'

# Type in a search box
curl -X POST http://localhost:8222/api/v1/tabs/{TAB_ID}/type \
  -H "Content-Type: application/json" \
  -d '{"text": "Show HN"}'
```

Every action returns a screenshot and event log automatically.

---

## What Makes ABP Different

### 1. Engine-Level Control

ABP embeds an HTTP server directly in the browser process. Requests are routed on the IO thread and dispatched on the UI thread with direct access to `Browser`, `TabStripModel`, and the DevTools agent. Zero inter-process overhead.

```
+---------------------------------------------------------+
|                  AI Agent (curl / Python / Go)          |
+----------------------------+----------------------------+
                             | REST API
                             v
+---------------------------------------------------------+
|              AbpHttpServer (IO thread)                  |
|              localhost:8222/api/v1/*                    |
+----------------------------+----------------------------+
                             | PostTask
                             v
+---------------------------------------------------------+
|              AbpController (UI thread)                  |
|   Direct access to Browser, TabStripModel, DevTools     |
+----------------------------+----------------------------+
                             |
              +--------------+--------------+
              v              v              v
         +--------+    +----------+    +--------+
         | Input  |    | Renderer |    |Network |
         | System |    |  (Blink) |    | Stack  |
         +--------+    +----------+    +--------+
```

### 2. Smart Action Response

Every action returns what the agent needs to make the next decision:

```json
{
  "result": {"x": 450, "y": 320, "button": "left"},
  "screenshot": {
    "data": "base64-webp...",
    "width": 1920,
    "height": 1080
  },
  "scroll": {
    "vertical_percent": 25.5,
    "page_height": 4700
  },
  "events": [
    {"type": "navigation", "data": {"url": "https://..."}}
  ]
}
```

No need to call "take screenshot" after every action. No need to poll for navigation events.

### 3. Execution Control

Freeze JavaScript execution between agent actions. The page stops. Timers freeze. `Date.now()` freezes. When you take a screenshot, you capture a deterministic state.

```bash
# Enable execution control
curl -X POST http://localhost:8222/api/v1/tabs/{id}/execution \
  -d '{"paused": true}'
```

Enabled by default with `--enable-abp`. Disable with `--abp-disable-pause`.

### 4. Element Markup

Every action endpoint accepts a `screenshot` object to control how the response screenshot is captured. Request bounding boxes drawn around interactive elements:

```bash
# Markup on a click action—see what's clickable after the click completes
curl -X POST http://localhost:8222/api/v1/tabs/{id}/click \
  -d '{"x": 450, "y": 320, "screenshot": {"markup": "interactive"}}'

# Markup on navigation—identify form fields on the new page
curl -X POST http://localhost:8222/api/v1/tabs/{id}/navigate \
  -d '{"url": "https://example.com", "screenshot": {"markup": "typeable"}}'

# Standalone screenshot with markup
curl -X POST http://localhost:8222/api/v1/tabs/{id}/screenshot \
  -d '{"screenshot": {"markup": "interactive"}}'
```

Markup options:
- `interactive` - All clickable and typeable elements
- `clickable` - Buttons, links, clickable elements
- `typeable` - Text inputs, textareas, contenteditable
- `inputs` - All form inputs

Screenshot options are available on all action endpoints: `/click`, `/type`, `/navigate`, `/scroll`, `/keyboard/*`, and `/screenshot`. The response includes element metadata with center coordinates for clicking.

### 5. Virtual Cursor

A compositor-layer cursor that moves with input actions and appears in screenshots. Your agent sees what a human would see.

### 6. Native Event Handling

File choosers, dialogs, and downloads are reported in the event stream:

```json
{
  "events": [
    {
      "type": "dialog",
      "data": {
        "dialog_type": "confirm",
        "message": "Delete this item?"
      }
    }
  ]
}
```

Handle them with dedicated endpoints:

```bash
curl -X POST http://localhost:8222/api/v1/tabs/{id}/dialog/accept
```

---

## API Reference

Base URL: `http://localhost:8222/api/v1`

### Browser and Tabs

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/browser/status` | Check if ABP is ready |
| GET | `/tabs` | List all tabs |
| GET | `/tabs/{id}` | Get tab details |
| POST | `/tabs` | Create new tab |
| DELETE | `/tabs/{id}` | Close tab |
| POST | `/tabs/{id}/activate` | Switch to tab |

### Navigation

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/tabs/{id}/navigate` | Go to URL |
| POST | `/tabs/{id}/back` | Navigate back |
| POST | `/tabs/{id}/forward` | Navigate forward |
| POST | `/tabs/{id}/reload` | Reload page |
| POST | `/tabs/{id}/stop` | Stop loading |

### Input

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/tabs/{id}/click` | Click at coordinates |
| POST | `/tabs/{id}/type` | Type text |
| POST | `/tabs/{id}/keyboard/press` | Press key (with modifiers) |
| POST | `/tabs/{id}/keyboard/down` | Key down |
| POST | `/tabs/{id}/keyboard/up` | Key up |
| POST | `/tabs/{id}/move` | Move mouse |
| POST | `/tabs/{id}/scroll` | Scroll (wheel) |

### Page Content

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/tabs/{id}/screenshot` | Get screenshot (binary) |
| POST | `/tabs/{id}/screenshot` | Get screenshot (base64 + metadata) |
| POST | `/tabs/{id}/execute` | Execute JavaScript |
| GET | `/tabs/{id}/content/html` | Get page HTML |
| GET | `/tabs/{id}/content/text` | Get page text |

### Events and Dialogs

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/tabs/{id}/dialog` | Get pending dialog |
| POST | `/tabs/{id}/dialog/accept` | Accept dialog |
| POST | `/tabs/{id}/dialog/dismiss` | Dismiss dialog |
| POST | `/file-chooser/{id}` | Provide files |

### Execution Control

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/tabs/{id}/execution` | Get execution state |
| POST | `/tabs/{id}/execution` | Pause/resume JavaScript |

### Downloads

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/downloads` | List downloads |
| GET | `/downloads/{id}` | Get download status |
| POST | `/downloads/{id}/cancel` | Cancel download |

See [plans/API.md](plans/API.md) for complete specification.

---

## MCP Server

The MCP server is embedded directly in Chrome—no separate process needed. It implements the MCP Streamable HTTP transport (protocol version 2025-03-26) at the `/mcp` endpoint.

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

Then ask Claude: "Go to news.ycombinator.com and find the top post about AI."

**Available tools (30 total):**

*Tab Management:* `browser_list_tabs`, `browser_new_tab`, `browser_close_tab`, `browser_get_tab_info`, `browser_activate_tab`, `browser_stop_loading`

*Navigation:* `browser_navigate`, `browser_go_back`, `browser_go_forward`, `browser_reload`

*Mouse:* `browser_click`, `browser_mouse_move`, `browser_scroll`

*Keyboard:* `browser_type`, `browser_keyboard_press`, `browser_keyboard_down`, `browser_keyboard_up`

*Screenshots:* `browser_screenshot`

*JavaScript:* `browser_execute_javascript`

*Dialogs:* `browser_get_dialog`, `browser_accept_dialog`, `browser_dismiss_dialog`

*Downloads:* `browser_list_downloads`, `browser_get_download`, `browser_cancel_download`

*Files:* `browser_provide_files`

*Execution Control:* `browser_get_execution_state`, `browser_set_execution_state`

*Browser:* `browser_get_status`, `browser_shutdown`

---

## Comparison

| Feature | ABP | CDP/Puppeteer | Playwright | Selenium |
|---------|-----|---------------|------------|----------|
| REST API | Yes | No (WebSocket) | No (RPC) | Yes |
| JS execution pause | Engine-level | Debugger | No | No |
| Virtual time | Yes | No | No | No |
| Virtual cursor | Compositor | No | No | No |
| Action screenshots | Automatic | Manual | Manual | Manual |
| Event detection | Built-in | Manual subscription | Manual | Manual |
| Element markup | Built-in | No | No | No |
| Engine integration | Native C++ | Protocol wrapper | Protocol wrapper | Protocol wrapper |

---

## Building from Source

### Prerequisites

1. Clone depot_tools:
   ```bash
   git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git ~/depot_tools
   export PATH="$HOME/depot_tools:$PATH"
   ```

2. Install build dependencies (Ubuntu/Debian):
   ```bash
   sudo ./build/install-build-deps.sh --no-prompt
   ```

### Build

```bash
# Sync dependencies
gclient sync --no-history

# Configure
gn gen out/Default --args='is_debug=true is_component_build=true symbol_level=1'

# Build
autoninja -C out/Default chrome
```

First build takes 4-6 hours. Incremental builds take seconds to minutes.

---

## Command Line Flags

| Flag | Description |
|------|-------------|
| `--enable-abp` | Enable ABP HTTP server |
| `--abp-port=8222` | API port (default: 8222) |
| `--abp-session-dir=PATH` | Session data directory |
| `--abp-disable-pause` | Disable automatic JS pause between actions |
| `--abp-auth-token=TOKEN` | Require bearer token authentication |
| `--abp-allow-remote` | Allow non-localhost connections |

---

## Project Structure

```
chrome/browser/abp/           # Core ABP implementation
  abp_http_server.cc/h        # HTTP server (IO thread)
  abp_controller.cc/h         # Request handling (UI thread)
  abp_mcp_handler.cc/h        # Embedded MCP server (JSON-RPC over HTTP)
  abp_switches.cc/h           # Command line flags

plans/                        # Design documents
  API.md                      # REST API specification
  agent-browser-protocol.md   # Architecture
  mcp.md                      # MCP specification
```

---

## Status

ABP is under active development. Current implementation:

**Working:**
- Tab management (list, create, close, activate)
- Navigation (URL, back, forward, reload, stop)
- Screenshots with element markup
- Mouse input (click, move, scroll)
- Keyboard input (type, press, key down/up)
- JavaScript execution
- Dialog handling (alert, confirm, prompt)
- File chooser support
- Download management
- Execution control (JS pause/resume, virtual time)
- History tracking with SQLite
- Virtual cursor rendering
- MCP server with 30 tools (full REST API parity)

**Not yet implemented:**
- Action success/failure tracking
- Revert URL to last known success state
- Revert browser to last known success state
- Recording of human browsing sessions as training data for agent fine-tuning

---

## Testing

ABP includes a comprehensive integration test suite validating core functionality.

### Run Tests

```bash
# Start Chrome with ABP
./out/Default/chrome --enable-abp

# Start test page server (separate terminal)
cd chrome/browser/abp/test_pages && python3 -m http.server 8081

# Run integration tests (10 test cases)
./run_tests.sh

# Run MCP server tests (8 test cases)
./tools/abp-mcp-test.sh
```

### Test Coverage

| Category | Tests |
|----------|-------|
| Navigation | URL navigation, back/forward |
| Input | Click, type, keyboard press |
| Screenshots | Capture with element markup |
| JavaScript | Execution and result retrieval |
| Execution Control | Virtual time freeze (3 tests) |
| MCP Server | Protocol compliance (8 tests) |

See [TESTING.md](TESTING.md) for the complete test matrix, test page documentation, and guide for adding new tests.

---

## Maintainers
* Han Wang ([@theredsix](https://github.com/theredsix))

## Contributing

ABP is a substantial fork of Chromium. Contributions welcome, please reach out to a maintainer about contributing.

---

## License

Chromium is licensed under the BSD 3-Clause License. ABP modifications follow the same license.

---

## Acknowledgments

ABP builds on the incredible work of the Chromium team. We're grateful for their commitment to open source. This fork was created with the assistance of Claude Code.
