# Agent Browser Protocol (ABP)

A Chromium fork implementing an agent-centric browser automation protocol designed from the ground up for AI agents.

## Vision

Current browser automation tools were built for humans debugging web applications, not for AI agents operating autonomously. The Agent Browser Protocol (ABP) inverts this paradigm: every design decision prioritizes what an AI agent needs to effectively control a browser.

### Why Existing Solutions Fall Short

**Chrome DevTools Protocol (CDP)** was designed for human developers using browser DevTools. Its complexity, session management overhead, and debugging-oriented design make it suboptimal for agent workloads.

**Browser Extensions** operate in a JavaScript sandbox with limited capabilities, security restrictions, and no access to browser internals.

**Selenium/Playwright** are wrappers around CDP or similar protocols, inheriting their limitations while adding abstraction overhead.

### The Agent-First Approach

ABP operates at the C++ engine level within Chromium itself, providing:

1. **Direct Engine Access**: No JavaScript bridges or extension sandboxes
2. **REST-First Interface**: Simple HTTP API any agent framework can consume
3. **Visual Feedback by Default**: Every action returns a screenshot and events
4. **Coordinate-Based Interaction**: Human-equivalent input through mouse/keyboard coordinates
5. **Event Awareness**: Agents learn about dialogs, downloads, popups, and navigation side-effects
6. **Element Markup**: Optional bounding boxes drawn on screenshots to help agents identify interactive targets

## Architecture

```
                     AI Agent / LLM
                          |
                          | HTTP/REST
                          v
+-------------------------------------------------------------+
|                   ABP REST Server                            |
|              (Embedded HTTP Server in Browser)               |
+-------------------------------------------------------------+
                          |
                          | Direct C++ Calls
                          v
+-------------------------------------------------------------+
|                  Browser Engine Core                         |
|  +----------+ +-----------+ +----------+ +--------------+    |
|  | Content  | |   Blink   | | Network  | |    Input     |    |
|  |  Shell   | |  Renderer | |  Stack   | |   System     |    |
|  +----------+ +-----------+ +----------+ +--------------+    |
+-------------------------------------------------------------+
```

ABP embeds an HTTP server directly in the browser process, handling REST requests on the IO thread and dispatching browser operations on the UI thread. This architecture eliminates inter-process communication overhead and provides direct access to browser internals.

## Core Principles

### 1. Engine-Native Control

All operations happen at the C++ level. Mouse clicks inject actual input events through the browser's input system. Screenshots capture the compositor output. JavaScript execution uses the same runtime as the page.

### 2. REST Interface

A simple HTTP API that any programming language or agent framework can use. No special SDKs, WebSocket handling, or protocol negotiation required.

### 3. Human-Equivalent Input

Agents interact through the same input channels as humans: x/y coordinates for mouse events, key codes for keyboard events. This ensures compatibility with all web content without relying on DOM manipulation.

### 4. Visual Response Envelope

Every action returns:
- **Screenshot**: WebP image of the viewport after the action completes
- **Events**: Browser events that occurred (navigation, dialogs, downloads)
- **Scroll Position**: Current scroll state and page dimensions
- **Timing**: Performance metrics for the action

This envelope provides agents with complete situational awareness after each interaction.

### 5. Element Markup

Optional bounding boxes can be drawn on screenshots to highlight interactive elements. Agents receive both the visual markup and structured data about each element's position, enabling coordinate-based clicking without complex DOM queries.

## Design Documents

| Document | Description |
|----------|-------------|
| [agent-browser-protocol.md](./agent-browser-protocol.md) | Core ABP architecture and design |
| [API.md](./API.md) | Complete REST API specification |
| [mcp.md](./mcp.md) | MCP server for AI agent integration |
| [implementation.md](./implementation.md) | Minimal implementation plan |
| [history.md](./history.md) | Action and event history tracking |

---

# REST API Specification

Base URL: `http://localhost:8222/api/v1`

## Authentication

If `--abp-auth-token` is set, all requests must include:

```
Authorization: Bearer <token>
```

---

## Standard Request Envelope

All action endpoints (POST/DELETE that modify state) accept standard parameters:

| Parameter | Type | Description |
|-----------|------|-------------|
| `wait_until` | object | When to capture the response |
| `screenshot` | object | Screenshot and markup options |

### Wait Until Options

| Type | Description |
|------|-------------|
| `immediate` | Return immediately after dispatching the action |
| `action_complete` | Wait for rendering/navigation lull (default) |
| `network_idle` | Wait until no network activity |
| `time` | Wait for a fixed duration |

**Parameters:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `type` | string | `action_complete` | Wait condition type |
| `timeout_ms` | number | 30000 | Maximum wait time |
| `idle_time_ms` | number | 500 | Idle duration for `network_idle` |
| `duration_ms` | number | - | Fixed wait for `time` type |

### Screenshot Options

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `area` | string | `viewport` | `none` or `viewport` |
| `markup` | string | `none` | Element markup overlay |
| `mouse` | string | `normal` | Cursor visibility |

**Markup values:**

| Value | Description |
|-------|-------------|
| `none` | No element markup |
| `interactive` | All interactive elements (clickable + typeable) |
| `clickable` | Buttons, links, clickable elements |
| `typeable` | Text inputs, textareas, contenteditable |
| `inputs` | All form inputs |

**Mouse values:**

| Value | Description |
|-------|-------------|
| `normal` | Show normal cursor |
| `none` | Hide cursor |
| `large` | Show cursor at 2x size |

---

## Standard Response Envelope

All action responses include:

| Field | Description |
|-------|-------------|
| `result` | Action-specific return data |
| `screenshot` | Viewport image after wait completion |
| `scroll` | Current scroll position and page dimensions |
| `events` | Browser events that occurred during the action |
| `timing` | Performance metrics |

### Screenshot Object

| Field | Type | Description |
|-------|------|-------------|
| `data` | string | Base64-encoded WebP image |
| `width` | number | Image width in pixels |
| `height` | number | Image height in pixels |
| `timestamp` | number | Capture timestamp |
| `markup` | string | Markup mode used |
| `marked_elements` | array | Elements marked in screenshot |

### Marked Elements

When markup is enabled, each element includes:

| Field | Type | Description |
|-------|------|-------------|
| `index` | number | Element index (matches label on screenshot) |
| `type` | string | `button`, `link`, `input`, `textarea`, `select`, `checkbox`, `radio` |
| `bounds` | object | Bounding box `{x, y, width, height}` |
| `center` | object | Center point `{x, y}` - use for clicking |
| `text` | string | Visible text content |
| `tag` | string | HTML tag name |
| `role` | string | ARIA role |
| `input_type` | string | Input type for `<input>` elements |
| `placeholder` | string | Placeholder text |
| `href` | string | Link URL for `<a>` elements |

### Scroll Position

| Field | Type | Description |
|-------|------|-------------|
| `horizontal_percent` | number | Horizontal position (0-100) |
| `vertical_percent` | number | Vertical position (0-100) |
| `horizontal_px` | number | Horizontal offset in pixels |
| `vertical_px` | number | Vertical offset in pixels |
| `page_width` | number | Total scrollable width |
| `page_height` | number | Total scrollable height |
| `viewport_width` | number | Visible viewport width |
| `viewport_height` | number | Visible viewport height |

### Event Types

Events captured between action dispatch and wait completion:

| Type | Description |
|------|-------------|
| `navigation` | Tab navigated to new URL |
| `dialog` | Alert/confirm/prompt appeared |
| `file_chooser` | Native file picker opened |
| `file_selected` | Files were selected |
| `file_chooser_cancelled` | File chooser dismissed |
| `popup` | New window/tab opened |
| `tab_closed` | Tab was closed |
| `scroll` | Page was scrolled |
| `download_started` | Download initiated |
| `download_completed` | Download finished |

---

## Browser Management

### Get Browser Info

```
GET /browser
```

**Response:**

| Field | Type | Description |
|-------|------|-------------|
| `browser` | string | Browser name |
| `version` | string | Browser version |
| `user_agent` | string | User agent string |
| `abp_version` | string | ABP version |
| `uptime_ms` | number | Uptime in milliseconds |

### Shutdown Browser

```
POST /browser/shutdown
```

**Request:**

| Field | Type | Description |
|-------|------|-------------|
| `timeout_ms` | number | Shutdown timeout |

---

## Tab Management

### List All Tabs

```
GET /tabs
```

**Response:**

| Field | Type | Description |
|-------|------|-------------|
| `tabs` | array | List of tab objects |
| `active_tab_id` | string | Currently active tab |
| `count` | number | Total tab count |

**Tab object:**

| Field | Type | Description |
|-------|------|-------------|
| `id` | string | Unique tab ID |
| `index` | number | Position in tab strip |
| `url` | string | Current URL |
| `title` | string | Page title |
| `active` | boolean | Is this the active tab |
| `pinned` | boolean | Is tab pinned |
| `audible` | boolean | Is tab playing audio |
| `muted` | boolean | Is tab muted |
| `loading` | boolean | Is page loading |
| `favicon_url` | string | Favicon URL |

### Get Tab Info

```
GET /tabs/{tab_id}
```

Returns detailed information including navigation state (`can_go_back`, `can_go_forward`), zoom level, and viewport bounds.

### Create New Tab

```
POST /tabs
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `url` | string | No | URL to navigate to |
| `active` | boolean | No | Make tab active (default: true) |
| `index` | number | No | Position in tab strip |

### Close Tab

```
DELETE /tabs/{tab_id}
```

### Activate Tab

```
POST /tabs/{tab_id}/activate
```

### Move Tab

```
POST /tabs/{tab_id}/move
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `index` | number | Yes | New position |

### Pin/Unpin Tab

```
POST /tabs/{tab_id}/pin
POST /tabs/{tab_id}/unpin
```

### Mute/Unmute Tab

```
POST /tabs/{tab_id}/mute
POST /tabs/{tab_id}/unmute
```

### Duplicate Tab

```
POST /tabs/{tab_id}/duplicate
```

---

## Navigation

### Navigate to URL

```
POST /tabs/{tab_id}/navigate
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `url` | string | Yes | URL to navigate to |
| `referrer` | string | No | Referrer URL |
| `wait_until` | object | No | Wait condition |

### Go Back

```
POST /tabs/{tab_id}/back
```

### Go Forward

```
POST /tabs/{tab_id}/forward
```

### Reload

```
POST /tabs/{tab_id}/reload
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `ignore_cache` | boolean | No | Bypass cache |

### Stop Loading

```
POST /tabs/{tab_id}/stop
```

---

## Mouse Actions

### Click

```
POST /tabs/{tab_id}/mouse/click
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `x` | number | Yes | X coordinate |
| `y` | number | Yes | Y coordinate |
| `button` | string | No | `left`, `right`, `middle` |
| `click_count` | number | No | 1=single, 2=double, 3=triple |
| `modifiers` | array | No | `["shift", "ctrl", "alt", "meta"]` |

### Mouse Down

```
POST /tabs/{tab_id}/mouse/down
```

Presses mouse button without releasing.

### Mouse Up

```
POST /tabs/{tab_id}/mouse/up
```

Releases mouse button.

### Mouse Move

```
POST /tabs/{tab_id}/mouse/move
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `x` | number | Yes | X coordinate |
| `y` | number | Yes | Y coordinate |
| `steps` | number | No | Intermediate steps for smooth movement |

### Drag and Drop

```
POST /tabs/{tab_id}/mouse/drag
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from_x` | number | Yes | Start X |
| `from_y` | number | Yes | Start Y |
| `to_x` | number | Yes | End X |
| `to_y` | number | Yes | End Y |
| `steps` | number | No | Movement steps |

### Scroll

```
POST /tabs/{tab_id}/mouse/scroll
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `x` | number | No | X position |
| `y` | number | No | Y position |
| `delta_x` | number | No | Horizontal scroll |
| `delta_y` | number | No | Vertical scroll (negative = down) |

### Hover

```
POST /tabs/{tab_id}/mouse/hover
```

Moves mouse to coordinates and waits to trigger hover states.

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `x` | number | Yes | X coordinate |
| `y` | number | Yes | Y coordinate |
| `duration_ms` | number | No | Hover duration |

---

## Keyboard Actions

### Type Text

```
POST /tabs/{tab_id}/keyboard/type
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `text` | string | Yes | Text to type |
| `delay_ms` | number | No | Delay between keystrokes |

### Press Key

```
POST /tabs/{tab_id}/keyboard/press
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `key` | string | Yes | Key to press |
| `modifiers` | array | No | Modifier keys |

**Common key values:**
- Letters: `a`-`z`, `A`-`Z`
- Numbers: `0`-`9`
- Function keys: `F1`-`F12`
- Navigation: `ArrowUp`, `ArrowDown`, `ArrowLeft`, `ArrowRight`
- Editing: `Backspace`, `Delete`, `Enter`, `Tab`, `Escape`
- Special: `Home`, `End`, `PageUp`, `PageDown`, `Insert`, `Space`

### Key Down

```
POST /tabs/{tab_id}/keyboard/down
```

Presses key without releasing.

### Key Up

```
POST /tabs/{tab_id}/keyboard/up
```

Releases a pressed key.

### Keyboard Shortcut

```
POST /tabs/{tab_id}/keyboard/shortcut
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `keys` | array | Yes | Keys to press together |

**Common shortcuts:**
- Select all: `["Control", "a"]`
- Copy: `["Control", "c"]`
- Paste: `["Control", "v"]`
- Undo: `["Control", "z"]`
- Find: `["Control", "f"]`

### Insert Text

```
POST /tabs/{tab_id}/keyboard/insert
```

Inserts text directly without key events. Useful for large text blocks.

---

## Page Content

### Get Page HTML

```
GET /tabs/{tab_id}/content/html
```

**Query params:**
- `outer=true` - Include `<html>` tag

### Get Page Text

```
GET /tabs/{tab_id}/content/text
```

### Get Page Title

```
GET /tabs/{tab_id}/content/title
```

### Get Page URL

```
GET /tabs/{tab_id}/content/url
```

### Execute JavaScript

```
POST /tabs/{tab_id}/content/execute
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `expression` | string | Yes | JavaScript to evaluate |
| `await_promise` | boolean | No | Wait for promise resolution |
| `timeout_ms` | number | No | Promise timeout |

**Supported return types:**
- Primitives: string, number, boolean, null, undefined
- Objects: Serialized as JSON
- Arrays: Serialized as JSON array
- Promises: Resolved value returned

---

## Screenshots

All screenshots are returned as WebP format.

### Get Screenshot

```
GET /tabs/{tab_id}/screenshot
```

**Query params:**
- `full_page=false` - Capture full scrollable page

**Response:** Binary WebP image data.

### Screenshot to Base64

```
POST /tabs/{tab_id}/screenshot
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `full_page` | boolean | No | Capture full page |
| `encoding` | string | No | `base64` |

### Region Screenshot

```
POST /tabs/{tab_id}/screenshot/region
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `x` | number | Yes | Left coordinate |
| `y` | number | Yes | Top coordinate |
| `width` | number | Yes | Width |
| `height` | number | Yes | Height |

### Screenshot with Wait

```
POST /tabs/{tab_id}/screenshot/wait
```

Captures screenshot after waiting for a condition. Returns the full action response envelope.

---

## Dialogs

### Get Pending Dialog

```
GET /tabs/{tab_id}/dialog
```

**Response:**

| Field | Type | Description |
|-------|------|-------------|
| `present` | boolean | Is a dialog pending |
| `type` | string | `alert`, `confirm`, `prompt`, `beforeunload` |
| `message` | string | Dialog message |
| `default_prompt` | string | Default prompt value |

### Accept Dialog

```
POST /tabs/{tab_id}/dialog/accept
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `prompt_text` | string | No | Text for prompt dialogs |

### Dismiss Dialog

```
POST /tabs/{tab_id}/dialog/dismiss
```

---

## Downloads

### Configure Downloads

```
POST /downloads/config
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `download_path` | string | Yes | Download directory |
| `prompt` | boolean | No | Show save dialog |
| `overwrite` | boolean | No | Overwrite existing files |

### List Downloads

```
GET /downloads
```

**Query params:**
- `state` - Filter: `in_progress`, `completed`, `cancelled`, `failed`
- `limit` - Max entries

### Get Download Info

```
GET /downloads/{download_id}
```

### Wait for Download

```
POST /downloads/wait
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `timeout_ms` | number | No | Maximum wait time |
| `state` | string | No | `started` or `completed` |

### Cancel Download

```
POST /downloads/{download_id}/cancel
```

### Resume Download

```
POST /downloads/{download_id}/resume
```

### Delete Download

```
DELETE /downloads/{download_id}
```

**Query params:**
- `delete_file=false` - Also delete the file

---

## File Chooser

Handle native OS file picker dialogs.

### Get Pending File Chooser

```
GET /tabs/{tab_id}/file-chooser
```

**Response:**

| Field | Type | Description |
|-------|------|-------------|
| `present` | boolean | Is a file chooser pending |
| `type` | string | `open`, `open-multiple`, `save` |
| `accepts` | array | Accepted file types |
| `multiple` | boolean | Can select multiple files |

### Select Files

```
POST /tabs/{tab_id}/file-chooser/select
```

**Request (open dialogs):**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `files` | array | Yes | File paths to select |

**Request (save dialogs):**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `path` | string | Yes | Save path |

### Cancel File Chooser

```
POST /tabs/{tab_id}/file-chooser/cancel
```

### Configure File Chooser

```
POST /tabs/{tab_id}/file-chooser/config
```

Pre-configure automatic file selection.

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `auto_select` | boolean | No | Auto-select without waiting |
| `default_files` | array | No | Default files for open dialogs |
| `default_save_path` | string | No | Default save directory |

---

## Network

### Get Network Log

```
GET /tabs/{tab_id}/network/requests
```

**Query params:**
- `limit` - Max entries
- `type` - Filter by resource type

### Enable Request Interception

```
POST /tabs/{tab_id}/network/intercept
```

### Get Intercepted Requests

```
GET /tabs/{tab_id}/network/intercepted
```

### Continue Intercepted Request

```
POST /tabs/{tab_id}/network/intercepted/{request_id}/continue
```

### Fulfill Intercepted Request

```
POST /tabs/{tab_id}/network/intercepted/{request_id}/fulfill
```

### Abort Intercepted Request

```
POST /tabs/{tab_id}/network/intercepted/{request_id}/abort
```

---

## Window Management

### Get Window Info

```
GET /window
```

### Set Window Bounds

```
POST /window/bounds
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `x` | number | No | X position |
| `y` | number | No | Y position |
| `width` | number | No | Width |
| `height` | number | No | Height |

### Minimize Window

```
POST /window/minimize
```

### Maximize Window

```
POST /window/maximize
```

### Fullscreen

```
POST /window/fullscreen
```

### Restore Window

```
POST /window/restore
```

---

## Wait Conditions

### Wait for Navigation

```
POST /tabs/{tab_id}/wait/navigation
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `wait_until` | string | No | `load`, `domcontentloaded`, `networkidle` |
| `timeout_ms` | number | No | Maximum wait time |

### Wait for Network Idle

```
POST /tabs/{tab_id}/wait/network-idle
```

**Request:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `idle_time_ms` | number | No | Required idle duration |
| `timeout_ms` | number | No | Maximum wait time |

---

## Error Codes

| Code | Description |
|------|-------------|
| `INVALID_REQUEST` | Malformed request body |
| `TAB_NOT_FOUND` | Tab ID does not exist |
| `ELEMENT_NOT_FOUND` | Element ID is stale |
| `SELECTOR_NOT_FOUND` | No element matches selector |
| `TIMEOUT` | Operation timed out |
| `NAVIGATION_FAILED` | Navigation could not complete |
| `ELEMENT_NOT_VISIBLE` | Element exists but not visible |
| `ELEMENT_NOT_INTERACTABLE` | Element cannot receive input |
| `DIALOG_NOT_PRESENT` | No dialog to handle |
| `FILE_CHOOSER_NOT_PRESENT` | No file chooser dialog |
| `FILE_NOT_FOUND` | File path does not exist |
| `FILE_NOT_READABLE` | File cannot be read |
| `DOWNLOAD_NOT_FOUND` | Download ID does not exist |
| `DOWNLOAD_FAILED` | Download could not complete |
| `NETWORK_ERROR` | Network operation failed |
| `EVALUATION_ERROR` | JavaScript evaluation failed |
| `UNAUTHORIZED` | Missing or invalid auth token |
| `RATE_LIMITED` | Too many requests |

---

## Command Line Flags

| Flag | Description |
|------|-------------|
| `--enable-abp` | Enable ABP HTTP server |
| `--abp-port=8222` | Port for ABP server |
| `--abp-auth-token=<token>` | Require bearer token authentication |
| `--abp-allow-remote` | Allow non-localhost connections |
| `--abp-cors-origin=<origin>` | Set allowed CORS origin |

---

## MCP Integration

ABP includes an MCP (Model Context Protocol) server that bridges AI agents to the REST API. See [mcp.md](./mcp.md) for the complete MCP tool specification.

The MCP server exposes all ABP functionality as tools that AI systems like Claude can call directly, enabling natural language browser control.

---

## Project Status

### Working Features

- Tab management (list, create, close, activate)
- Navigation (URL, back, forward, reload)
- Screenshots (viewport with optional element markup)
- Input (click, type via CDP)
- JavaScript execution (via CDP)

### In Development

- History tracking with SQLite persistence
- Full mouse action support
- Full keyboard action support
- Download management
- File chooser handling
- Network interception
