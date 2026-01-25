# Agent Browser Protocol

**A Chromium fork with a REST API built directly into the browser engine for AI agent control.**

**Synchronous browsing for agents:** one request = one completed step (settled state + screenshot + event log).

```
   Your AI Agent                 ABP Chromium
       |                             |
       |   POST /tabs/1/click       |
       |   {"x": 450, "y": 320}     |
       |--------------------------->|
       |                            | [injects real mouse event]
       |                            | [waits for page to settle]
       |                            | [captures screenshot]
       |   {                        |
       |     "screenshot": "...",   |
       |     "events": [...]        |
       |   }                        |
       |<---------------------------|
```

No WebSocket. No CDP session management. No Puppeteer abstraction layers.
Just `curl http://localhost:8222/api/v1/tabs` and you're in.

---

## Why Fork Chromium?

Extensions run in a sandbox. CDP was designed for DevTools, not autonomous control. Playwright and Puppeteer inherit that model: a live, asynchronous browser where agents must guess when an action is complete, juggle sessions, and paper over timing with retries.

We needed **synchronous browsing for agents**: a step-based, request/response contract where the agent only ever acts on a stable world state.

| What agents need | What existing tools provide |
|------------------|----------------------------|
| Pause JavaScript between actions | Debugging pause (breaks the page) |
| Pause time between actions | Real-time only |
| Compositor-layer cursor rendering | No cursor visibility |
| Simple REST API | WebSocket + session management |
| Engine-level event injection | DOM simulation or CDP passthrough |
| Action-complete detection | Manual waits or flaky heuristics |
| Event list between actions (new tab, dialog, file picker, etc.) | Polling, or async event subscriptions |

**ABP treats the browser as a step machine.** Each API call injects real input through Chromium's input system, waits for an engine-defined "settled" boundary, captures compositor output (with cursor), and returns the events that occurred during the step. JavaScript and virtual time are paused between steps—so the agent experiences a synchronous, deterministic control surface over an inherently asynchronous web.

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

# Take a screenshot with interactive elements marked
curl -X POST http://localhost:8222/api/v1/tabs/{TAB_ID}/screenshot \
  -H "Content-Type: application/json" \
  -d '{"screenshot": {"markup": "interactive"}}'

# Click the first link
curl -X POST http://localhost:8222/api/v1/tabs/{TAB_ID}/click \
  -H "Content-Type: application/json" \
  -d '{"x": 450, "y": 320}'

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

Request screenshots with bounding boxes drawn around interactive elements:

```bash
curl -X POST http://localhost:8222/api/v1/tabs/{id}/screenshot \
  -d '{"screenshot": {"markup": "interactive"}}'
```

Markup options:
- `interactive` - All clickable and typeable elements
- `clickable` - Buttons, links, clickable elements
- `typeable` - Text inputs, textareas, contenteditable
- `inputs` - All form inputs

The response includes element metadata with center coordinates for clicking.

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

Connect Claude or any MCP-compatible AI directly to ABP:

```bash
cd tools/abp-mcp-server
npm install && npm run build
npm start
```

Configure in Claude Desktop (`claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "browser": {
      "command": "node",
      "args": ["/path/to/abp-mcp-server/dist/index.js"],
      "env": {"ABP_URL": "http://localhost:8222"}
    }
  }
}
```

Then ask Claude: "Go to news.ycombinator.com and find the top post about AI."

---

## Comparison

| Feature | ABP | CDP/Puppeteer | Playwright | Selenium |
|---------|-----|---------------|------------|----------|
| REST API | Yes | No (WebSocket) | No (RPC) | Yes |
| JS execution pause | Engine-level | Debugger (breaks page) | No | No |
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
  abp_switches.cc/h           # Command line flags

tools/abp-mcp-server/         # MCP server (TypeScript)
  src/index.ts                # Tool definitions and handlers

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
- MCP server with 14 tools

**Not yet implemented:**
- Action success/failure tracking
- Revert URL to last known success state
- Revert browser to last known success state

---

## Contributing

ABP is a substantial fork of Chromium. Contributions welcome, but please:

1. Open an issue first to discuss major changes
2. Follow Chromium's coding style for C++ code
3. Add tests for new functionality

---

## License

Chromium is licensed under the BSD 3-Clause License. ABP modifications follow the same license.

---

## Acknowledgments

ABP builds on the incredible work of the Chromium team. We're grateful for their commitment to open source. This fork was created with the assistance of Claude Code.
