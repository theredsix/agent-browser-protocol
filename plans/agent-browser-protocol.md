# Agent Browser Protocol - Technical Design Spec

## Implementation Status

This document describes the ABP architecture and design. The actual implementation is located at `chrome/browser/abp/` (not `//components/abp_server/` as originally planned).

**Core Implementation Complete:**
- AbpHttpServer (IO thread HTTP server)
- AbpController (UI thread request handler)
- AbpActionContext (unified action flow)
- AbpEventCollector (event capture)
- AbpHistoryController (SQLite persistence)
- Virtual cursor (compositor layer via Mojo IPC)
- Execution control (pause/resume + virtual time)

See [implementation.md](./implementation.md) for detailed implementation status.

---

## Overview

The Agent Browser Protocol (ABP) is a REST-based API that provides engine-native browser control for AI agents. Unlike CDP (Chrome DevTools Protocol) or browser extensions, ABP operates at the C++ engine level, offering direct access to browser internals with lower latency and greater capability.

For complete API reference, see [API.md](./API.md).

## Goals

- **Engine-Native Control**: Direct C++ integration bypassing JavaScript/extension sandboxes
- **REST Interface**: Simple HTTP-based API for agent consumption
- **Low Latency**: Minimal overhead for real-time agent interactions
- **Full Browser Access**: Capabilities beyond what CDP/extensions can provide
- **Human-Equivalent Input**: All keyboard and mouse actions a human can perform
- **Event-Driven Response**: Every action returns screenshot + events for agent awareness
- **Security Isolation**: Controlled access patterns for safe agent operation

## Non-Goals

- Compatibility with existing CDP tooling
- Browser extension support for ABP features
- Multi-tenant/shared browser scenarios (single agent per instance)

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Agent (Client)                         │
└─────────────────────────┬───────────────────────────────────┘
                          │ HTTP/REST
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                   ABP REST Server                           │
│              (Embedded HTTP Server in Browser)              │
└─────────────────────────┬───────────────────────────────────┘
                          │ Direct C++ Calls
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                  Browser Engine Core                        │
│  ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌─────────────┐  │
│  │  Content  │ │   Blink   │ │  Network  │ │    Input    │  │
│  │   Shell   │ │  Renderer │ │   Stack   │ │   System    │  │
│  └───────────┘ └───────────┘ └───────────┘ └─────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## Core Components

### 1. ABP Server (`//components/abp_server/`)

Embedded HTTP server handling REST requests. Built on Chromium's network stack.

**Responsibilities:**
- Listen on configurable port (default: 8222)
- Parse/validate REST requests
- Route to appropriate controllers
- Serialize responses as JSON

### 2. Browser Controller (`//components/abp_server/controllers/browser_controller`)

Global browser state and lifecycle management.

- Browser info and version
- Window management (bounds, minimize, maximize, fullscreen)
- Graceful shutdown

### 3. Tab Controller (`//components/abp_server/controllers/tab_controller`)

Manages tab lifecycle and meta-operations.

- Create, close, duplicate tabs
- Switch between tabs (activate)
- Get tab info (URL, title, loading state, favicon)
- Move, pin, mute tabs
- Navigation (URL, back, forward, reload, stop)

### 4. Mouse Controller (`//components/abp_server/controllers/mouse_controller`)

Engine-level mouse input injection replicating all human mouse actions.

- Click (left, right, middle button; single, double, triple click)
- Mouse down/up (for drag operations)
- Mouse move (with configurable steps for smooth movement)
- Drag and drop
- Scroll (wheel events with delta)
- Hover (move + wait for hover states)
- Modifier keys (Shift, Ctrl, Alt, Meta with clicks)

### 5. Keyboard Controller (`//components/abp_server/controllers/keyboard_controller`)

Engine-level keyboard input injection replicating all human keyboard actions.

- Type text (with configurable delay between keystrokes)
- Press key (single key down + up)
- Key down/up (for held keys)
- Keyboard shortcuts (modifier + key combinations)
- Raw text insertion (bypass key events for large text)
- Full key support (letters, numbers, F-keys, navigation, editing, special keys)

### 6. Content Controller (`//components/abp_server/controllers/content_controller`)

JavaScript execution for reading page state.

- Execute JavaScript expressions and retrieve results

Note: JavaScript execution is for reading state/extracting data. All interactions remain coordinate-based.

### 7. Screenshot Controller (`//components/abp_server/controllers/screenshot_controller`)

Visual capture at the engine level.

- Full viewport screenshot
- Full page screenshot (scrolled)
- WebP format at quality 80

### 8. Dialog Controller (`//components/abp_server/controllers/dialog_controller`)

Handle browser dialogs (alert, confirm, prompt).

- Get pending dialog info
- Accept/dismiss dialogs
- Provide prompt input

### 9. Download Controller (`//components/abp_server/controllers/download_controller`)

Read-only access to download status. Configuration via ABP config at launch.

- List downloads with filtering by state
- Get download status by ID
- Cancel in-progress downloads

### 10. File Chooser Handler

File chooser dialogs are tracked with unique IDs and handled via a dedicated endpoint.

- Actions that trigger file choosers return a `file_chooser` event with unique `id`
- Use `POST /file-chooser/{id}` to provide files or cancel the dialog
- Events include `file_chooser`, `file_selected`, and `file_chooser_cancelled`
- Default behavior (auto-accept) can be configured via ABP config at launch

### 11. Event Collector (`//components/abp_server/event_collector`)

Captures browser events during action execution for response envelope.

- Subscribe to navigation, dialog, file chooser, scroll events
- Track popup window/tab creation
- Monitor tab closures
- Track download start/completion
- Buffer events between action start and wait completion

### 12. Wait Controller (`//components/abp_server/wait_controller`)

Implements wait_until semantics for action completion detection.

- `action_complete` heuristic using engine signals
- Timeout handling
- Rendering quiescence detection via compositor

## Response Envelope Design

All action endpoints return a standard envelope containing:

1. **Result**: Action-specific return data
2. **Screenshot**: Compressed WebP image with `virtual_time_ms` (frozen page time)
3. **Scroll**: Current scroll position after wait completion
4. **Events**: Array of browser events with `virtual_time_ms` timestamps
5. **Timing**: Performance metrics for the action

Note: All timestamps use `virtual_time_ms` (virtual time in milliseconds since epoch). Since execution is paused between actions, this reflects the frozen page time rather than wall clock time.

### Wait Until Semantics

Actions accept a `wait_until` parameter controlling when to capture the response:

| Type | Description |
|------|-------------|
| `immediate` | Return right after action dispatch |
| `action_complete` | Wait for rendering/navigation lull (default) |
| `time` | Fixed duration wait |

The `action_complete` heuristic monitors:
- Rendering quiescence (no pending paint/layout)
- Navigation settled (no pending loads/redirects)
- Script idle (no pending tasks/promises)
- Minimum delay after last activity

### Event Capture

Events captured during wait period inform agents of side effects:

- **navigation**: Page navigated to new URL
- **dialog**: Alert/confirm/prompt appeared (includes dialog ID)
- **file_chooser**: Native file picker opened (includes `id` for `POST /file-chooser/{id}`)
- **file_selected**: Files were selected in file chooser (includes `id`)
- **file_chooser_cancelled**: File chooser dismissed without selection (includes `id`)
- **popup**: New window/tab opened
- **tab_closed**: Tab was closed
- **scroll**: Page was scrolled (includes delta, final position, source)
- **download_started**: Download initiated (includes `download_id`)
- **download_completed**: Download finished successfully (includes `download_id`)

### Scroll Position

Every action response includes current scroll state:

- **horizontal_percent / vertical_percent**: Position as percentage (0-100)
- **horizontal_px / vertical_px**: Position in pixels
- **page_width / page_height**: Total scrollable dimensions
- **viewport_width / viewport_height**: Visible viewport size

This design allows agents to:
1. See the visual result of every action
2. Know exact scroll position to understand visible content
3. Detect dialogs/file choosers requiring handling
4. Track file selection and download completion
5. Track navigation, scroll, and popup side effects

## Security Model

### Localhost Binding
ABP server binds to `127.0.0.1` only by default. Remote access requires explicit flag.

### Authentication
Optional bearer token authentication via `--abp-auth-token` flag.

### Request Limits
Rate limiting and request size limits to prevent resource exhaustion.

## Command Line Flags

```
--enable-abp                    Enable Agent Browser Protocol server
--abp-port=8222                 Port for ABP server (default: 8222)
--abp-session-dir=<path>        Session directory for screenshots, database, logs
--abp-disable-pause             Disable execution control (Debugger.pause + virtual time)
--allow-system-inputs           Allow system input when ABP is enabled (blocked by default)
```

## ABP Configuration

Window sizing and download behavior are configured via the ABP config file (`--abp-config` or `~/.config/chromium/abp_config.json`):

```json
{
  "window": {
    "width": 1280,
    "height": 720,
    "x": 0,
    "y": 0
  },
  "downloads": {
    "path": "/tmp/downloads",
    "auto_accept": true
  },
  "file_chooser": {
    "default_files": [],
    "default_save_path": "/tmp/saves"
  }
}
```

## Implementation Phases

### Phase 1: Foundation ✅
- [x] Embedded HTTP server infrastructure
- [x] Tab controller (create, close, list, info)
- [x] Basic navigation (URL, back, forward, reload)
- [x] Screenshot capture

### Phase 2: Human Input (in progress)
- [x] Mouse click
- [x] Mouse move with virtual cursor
- [ ] Mouse scroll
- [x] Keyboard type
- [ ] Keyboard press, down, up
- [ ] Tab activate (switch)

### Phase 3: Content & Execution ✅
- [x] JavaScript evaluation
- [x] Execution control (pause/resume)
- [x] Virtual time management

### Phase 4: Dialogs & Downloads
- [ ] Dialog handling (get, accept, dismiss)
- [ ] Download status (list, get, cancel)
- [ ] File chooser integration in actions

### Phase 5: History & Debugging ✅
- [x] Action recording with before/after screenshots
- [x] SQLite history database
- [x] Session directory management

## File Structure (Actual Implementation)

```
chrome/browser/abp/
├── BUILD.gn
├── abp_switches.h/cc            # Command line flags
├── abp_config.h/cc              # Configuration handling
├── abp_http_server.h/cc         # HTTP server (IO thread)
├── abp_controller.h/cc          # Request handler + CDP client (UI thread)
├── abp_history_controller.h/cc  # Action recording
├── abp_history_database.h/cc    # SQLite storage
├── abp_event_observer.h/cc      # Browser event monitoring
├── abp_mouse_tracker.h/cc       # Virtual cursor state
└── abp_action_context.h/cc      # Action flow management
```

## Dependencies

- `//net` - Network stack for HTTP server
- `//content/public/browser` - Browser-side content APIs
- `//ui/gfx` - Graphics/screenshot utilities
- `//third_party/libwebp` - WebP screenshot compression
- `//sql` - SQLite for history database
- `//base` - Base utilities and threading

## Related Documents

- [API.md](./API.md) - Complete REST API specification
- [mcp.md](./mcp.md) - MCP server for AI agent integration
