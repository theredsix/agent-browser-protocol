<h1 align="center">Agent Browser Protocol</h1>

**Web browsing is continuous and async. Agents think in tools and steps. ABP reformats web navigation into the discrete, multimodal chat format agents know and love.**

<p align="center"><strong>90.53% on Online Mind2Web</strong> — <a href="https://github.com/theredsix/abp-online-mind2web-results">reproducible results</a></p>

---

- **2x lower token usage**
- **2x faster automation runs**
- **2x lower tool calls**

_*compared to Playwright MCP_

---

ABP is a Chromium fork with **MCP + REST** baked directly into the browser engine.

- **One request = one completed step**: settled state + screenshot + event log
- **No WebSocket. No CDP session management.** Just HTTP.
- **~100ms overhead per action** (including screenshots). The bottleneck is the LLM, not the browser.

> **Try it in 60 seconds (Claude Code)**
>
> ```bash
> # 1) Add ABP as an MCP server to Claude Code
> claude mcp add browser -- npx -y agent-browser-protocol --mcp
>
> # 2) Sanity check the server is up (optional)
> curl -s http://localhost:8222/api/v1/tabs
> ```
>
> Wait for the browser to launch and ask Claude:
>
> - “Find me kung pao chicken near 415 Mission St, San Francisco on Doordash.”
>
> **What you should notice:** every tool call returns a settled page state (screenshot + events), and the page freezes between steps so Claude never races the browser.

![ABP - New Tab - 25 February 2026 (1)](https://github.com/user-attachments/assets/6256ecd8-f9c4-482e-b2e0-533e4e43cd40)

---

## What you get per action

```
AI Agent                                 ABP Chromium
    │                                         │
    │  POST /click (x=450, y=320)             │
    │────────────────────────────────────────>│
    │                                         │  Inject real input event
    │                                         │  Wait for page to settle
    │                                         │  Capture compositor screenshot
    │                                         │  Collect events (tab_created, dialog, file_chooser…)
    │                                         │  Pause JavaScript + virtual time
    │  200 OK: screenshot + events            │
    │<────────────────────────────────────────│
    │
    ·  (agent inspects screenshot, decides)   ·
    │
    │  POST /type (text="Show HN")            │
    │────────────────────────────────────────>│
    │                                         │  Unpause JS + time
    │                                         │  Inject real keyboard events
    │                                         │  Wait for settle → screenshot → events → pause
    │  200 OK: screenshot + events            │
    │<────────────────────────────────────────│
```

---

## Quick Start

> **Note:** If you have a Playwright MCP server configured, disable it before using ABP to avoid tool name conflicts.

### Claude Code

```bash
claude mcp add browser -- npx -y agent-browser-protocol --mcp
```

Then ask Claude: *"Go to news.ycombinator.com and find the top post about AI."*

### Codex CLI

```bash
codex mcp add browser -- npx -y agent-browser-protocol --mcp
```

### Opencode

Configure a model with vision and add the MCP server.

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "browser": {
      "type": "local",
      "command": ["npx", "-y", "agent-browser-protocol", "--mcp"],
      "enabled": true,
      "environment": {
      }
    }
  }
}
```

### Any MCP Client (HTTP)

Launch ABP:

```bash
npx -y agent-browser-protocol
```

Then point your MCP client at `http://localhost:8222/mcp` (streamable HTTP).

For example, in Claude Desktop (`claude_desktop_config.json`):

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

### REST (no MCP)

Launch ABP:

```bash
npx -y agent-browser-protocol
```

Then drive it with curl:

```bash
# List tabs
curl -s http://localhost:8222/api/v1/tabs

# Navigate (returns screenshot + events)
# Make sure you replace <TAB_ID> with an actual tab_id from above
curl -s -X POST http://localhost:8222/api/v1/tabs/<TAB_ID>/navigate \
  -H 'content-type: application/json' \
  -d '{"url":"https://example.com","screenshot":{"format":"webp"}}'
```

See [docs/REST-API.md](docs/REST-API.md) for curl examples and the full API reference.

> **npm package details?** See [theredsix/abp-npm](https://github.com/theredsix/abp-npm) for the TypeScript SDK, plugin config, and debug server.
>
> **Manual binary download?** See [MANUAL_INSTALL.md](MANUAL_INSTALL.md) for direct download and launch instructions.
>
> **Building from source?** See [COMPILE.md](COMPILE.md) for macOS, Linux, and Windows.
>
> **Uninstalling?** See [Uninstall](#uninstall).

---

## ABP in Action

Short demo: Use google maps and find a route from Seattle to LA by train.

https://github.com/user-attachments/assets/739d13ac-193a-4910-b347-7493f6da15a4

Notice the freezing of the spinners while the LLM is thinking. ABP pauses JavaScript and virtual time between actions so the page waits for the agent.

---

## Why ABP (and why a Chromium build)

The core problem is a mismatch:
* Web browsing is continuous and asynchronous
* LLM agents reason step-by-step

Most automation stacks force agents to race against a live browser, then patch over the mismatch with waits and retries.

ABP makes browsing a step machine. Each request injects native input, waits for an engine-defined “settled” boundary, captures compositor output (with cursor), returns an event log, then freezes JavaScript + virtual time until the next step.

**ABP reformats browsing into a step machine**: a request/response contract where the agent only ever acts on a stable, frozen world state.

| What agents need | What existing tools provide |
|------------------|----------------------------|
| Deterministic step boundary (“settled”) | Manual waits, heuristics |
| Pause time between actions | Real-time only |
| Screenshot on every step (with cursor) | Extra calls, no cursor |
| Simple REST API | WebSocket + session management |
| Engine-level event injection | DOM simulation or CDP passthrough |
| Dialog/file chooser/download surfaced as events | Polling or async subscriptions |

**Each API call is one atomic step.** ABP injects real input through Chromium's input system, waits for an engine-defined "settled" boundary, captures compositor output (with cursor), and returns the events that occurred. JavaScript and virtual time freeze between steps. The agent never races against the browser—it observes, decides, acts, and repeats on a world that waits for it.

---

Docs

* TypeScript SDK + npm details: [README](tools/abp-npm/README.md)
* REST API reference + curl examples: [REST-API.md](docs/REST-API.md)
* Manual binary download + launch: [MANUAL_INSTALL.md](MANUAL_INSTALL.md)
* Building from source: [COMPILE.md](COMPILE.md)
* Training / SQLite session schema: [TRAINING.md](TRAINING.md)

---

Security notes

* ABP is intended to run locally on your machine.
* The API is served on localhost by default (--abp-port=8222).
* ABP blocks real system input by default; use --allow-system-inputs to override.

---


## What Makes ABP Different

### 1. Engine-Level Control

ABP embeds an HTTP server directly in the browser process. Requests are routed on the IO thread and dispatched on the UI thread with direct access to `Browser`, `TabStripModel`, and the DevTools agent.

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
  "result": {"status": "clicked"},
  "screenshot_before": {
    "data": "base64-webp...",
    "width": 1920, "height": 1080
  },
  "screenshot_after": {
    "data": "base64-webp...",
    "width": 1920, "height": 1080
  },
  "scroll": {"scrollX": 0, "scrollY": 150, "pageWidth": 1280, "pageHeight": 4000, "viewportWidth": 1280, "viewportHeight": 720},
  "events": [
    {"type": "navigation", "virtual_time_ms": 0, "data": {"tab_id": "...", "url": "https://...", "frame_id": "...", "is_main_frame": true}},
    {"type": "dialog", "virtual_time_ms": 0, "data": {"tab_id": "...", "dialog_type": "confirm", "message": "Delete this item?"}},
    {"type": "file_chooser", "virtual_time_ms": 0, "data": {"id": "fc_1", "tab_id": "...", "chooser_type": "open", "multiple": false, "accepts": [".pdf", ".docx"], "pending": true}}
  ],
  "timing": {"action_started_ms": 1700000000000, "action_completed_ms": 1700000000050, "duration_ms": 50},
  "cursor": {"x": 450, "y": 320, "cursor_type": "pointer"}
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

Enabled by default. Disable with `--abp-disable-pause`.

### 4. Element Markup

Request bounding boxes drawn around interactive elements in any action's response screenshot:

```bash
# Markup on a click action
curl -X POST http://localhost:8222/api/v1/tabs/{id}/click \
  -d '{"x": 450, "y": 320, "screenshot": {"markup": ["clickable", "typeable"]}}'

# Markup on navigation
curl -X POST http://localhost:8222/api/v1/tabs/{id}/navigate \
  -d '{"url": "https://example.com", "screenshot": {"markup": ["typeable"]}}'
```

Markup options: `clickable`, `typeable`, `scrollable`, `grid`, `selected`.

### 5. Virtual Cursor

A compositor-layer cursor that moves with input actions and appears in screenshots. Your agent sees what a human would see.

### 6. Native Event Handling

File choosers, dialogs, and downloads are reported in the event stream:

```json
{
  "events": [
    {"type": "dialog", "data": {"tab_id": "...", "dialog_type": "confirm", "message": "Delete this item?"}}
  ]
}
```

Handle them with dedicated endpoints:

```bash
curl -X POST http://localhost:8222/api/v1/tabs/{id}/dialog/accept
```

### 7. Session Recording for Agent Training

Every action is recorded to a SQLite database with before/after screenshots, parameters, results, timing, and success/failure status. Successful agent sessions become fine-tuning datasets for vision-language models.

```
Action #1: navigate("https://example.com")
  ├── screenshot_before.webp
  ├── params: {"url": "https://example.com"}
  └── screenshot_after.webp

Action #2: click(450, 320)
  ├── screenshot_before.webp
  ├── params: {"x": 450, "y": 320}
  └── screenshot_after.webp
```

Control session storage with `--abp-session-dir`:

```bash
./abp --abp-session-dir=./datasets/session-001
```

See [TRAINING.md](TRAINING.md) for the SQLite schema, `abp-debug` UI, and training pipeline examples.

---

## Comparison

| Feature | ABP | CDP/Puppeteer | Playwright | Selenium | [agent-browser](https://github.com/vercel-labs/agent-browser) |
|---------|-----|---------------|------------|----------|----------------|
| REST API | Yes | No (WebSocket) | No (RPC) | Yes | No (CLI) |
| JS execution pause | Engine-level | Debugger | No | No | No |
| Virtual time | Yes | Partial (CDP only) | Partial (Clock API) | No | No |
| Virtual cursor | Compositor | No | No | No | No |
| Action screenshots | Automatic | Manual | Manual | Manual | Manual (CLI flag) |
| Event detection | Built-in | Manual subscription | Manual | Manual | No |
| Element markup | Built-in | No | No | No | Annotated screenshots |
| Session recording | Built-in | DevTools Recorder | Codegen + Trace | Selenium IDE | No |
| Engine integration | Native C++ | Protocol wrapper | Protocol + browser patches | Protocol wrapper | CDP wrapper (Rust) |
| Runtime.enable required | No | Yes | Yes | N/A | Yes |
| Input dispatch | Native (RenderWidgetHost) | CDP synthetic (Input.dispatch*) | CDP/Juggler synthetic | WebDriver → CDP synthetic | CDP synthetic |
| Scroll method | Native wheel events | CDP Input.dispatchMouseEvent | CDP or JS scrollIntoView | JS or Actions API | CDP synthetic |
| Compositor hit-testing | Yes (full input pipeline) | No (bypasses compositor) | No | No | No |
| Blocks real user input | Yes (default) | No | No | No | No |

---

## Command Line Flags

| Flag | Description |
|------|-------------|
| `--abp-port=8222` | API port (default: 8222) |
| `--abp-session-dir=PATH` | Session data directory (default: /tmp/abp-UUID) |
| `--abp-config=PATH` | Config file path |
| `--abp-window-size=W,H` | Window size (default: 1280,887) |
| `--abp-zoom=FACTOR` | Zoom factor (default: 1.0) |
| `--abp-disable-pause` | Disable automatic JS pause between actions |
| `--allow-system-inputs` | Allow system input (ABP blocks by default) |

---

## Project Structure

```
chrome/browser/abp/                 # Core ABP implementation
  abp_http_server.cc/h              # HTTP server (IO thread)
  abp_controller.cc/h               # Request handling (UI thread)
  abp_action_context.cc/h           # Action lifecycle (pause/resume/screenshot)
  abp_input_dispatcher.cc/h         # Native input dispatch (click/scroll/keys)
  abp_event_observer.cc/h           # CDP event client per tab
  abp_event_collector.cc/h          # Event collection during actions
  abp_mcp_handler.cc/h              # Embedded MCP server (JSON-RPC over HTTP)
  abp_tool_builder.cc/h             # MCP tool schema builder
  abp_history_controller.cc/h       # Session/action history API
  abp_history_database.cc/h         # SQLite history storage
  abp_download_observer.cc/h        # Download tracking
  abp_config.cc/h                   # Runtime configuration
  abp_types.h                       # Shared type definitions
  abp_switches.cc/h                 # Command line flags

plans/                              # Design documents
  API.md                            # REST API specification
  agent-browser-protocol.md         # Architecture
  mcp.md                            # MCP specification
```

---

## Status

ABP is under active development. Current implementation:

**Working:**
- Tab management (list, create, close, activate, stop)
- Navigation (URL, back, forward, reload)
- Screenshots with element markup and virtual cursor
- Mouse input (click, move, drag, scroll via native wheel events)
- Keyboard input (type, press, key down/up with modifiers)
- JavaScript execution
- Text extraction (full page or CSS selector)
- Input helpers (slider, clear-text)
- Duration and network wait
- Dialog handling (alert, confirm, prompt, beforeunload)
- File chooser support (local files and base64 content)
- Native select popup handling
- Download management (list, status, cancel, content retrieval)
- Permission prompt handling + geolocation spoofing
- Execution control (JS pause/resume, virtual time)
- History tracking with SQLite (sessions, actions, events)
- Virtual cursor rendering (compositor layer)
- Browser management (status, shutdown)
- Embedded MCP server with 18 tools at `/mcp`
- Console MCP actions

**Not yet implemented:**
- Action success/failure tracking
- Recording of human browsing sessions as training data for agent fine-tuning
- Full headless support

---

## Testing

ABP includes integration tests validating core functionality including navigation, input, screenshots, JavaScript execution, execution control, and MCP protocol compliance.

See [TESTING.md](TESTING.md) for the complete test matrix, test page documentation, and guide for adding new tests.

## REST API

ABP also exposes a full REST API for direct HTTP integration. See [docs/REST-API.md](docs/REST-API.md) for the quick start and complete endpoint reference.

## Maintainers
* Han Wang ([@theredsix](https://github.com/theredsix))

## Uninstall

Remove the MCP server from your client:

```bash
# Claude Code
claude mcp remove browser

# Codex CLI
codex mcp remove browser
```

For other clients, delete the `browser` entry from your MCP configuration file.

## Contributing

ABP is a substantial fork of Chromium. Contributions welcome, please reach out to a maintainer about contributing.

## License

Copyright 2026 Han Wang. All rights reserved.

Chromium is licensed under the BSD 3-Clause License. ABP modifications are Copyright 2026 Han Wang and follow the same license.

## Acknowledgments

ABP builds on the incredible work of the Chromium team. We're grateful for their commitment to open source. This fork was created with the assistance of Claude Code. We're also extremely appreciative for our sponsors for their generousity.

## Sponsors
* [Skyvern](https://github.com/Skyvern-AI/skyvern/blob/main/docs/images/skyvern_logo_blackbg.png)
![Skyvern](./sponsors/skyvern.png)
