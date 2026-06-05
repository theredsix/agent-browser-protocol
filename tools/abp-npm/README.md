# agent-browser-protocol

Deterministic AI agent browser control at the engine level. A Chromium fork where every action is atomic: input, wait for settle, screenshot, pause. No race conditions.

## Install

```bash
npm install agent-browser-protocol
```

Downloads the pre-built ABP browser binary for your platform (~130MB) on first install.

## What's Inside

This package provides three things:

1. **REST API client** — typed TypeScript SDK for the 50+ endpoint ABP REST API
2. **MCP server** — 21 tools for AI-assisted browsing (Claude Code, Codex, or any MCP client)
3. **Debug server** — web UI for inspecting session history, screenshots, and action logs

---

## 1. REST API Client

Launch ABP and control it programmatically with zero dependencies beyond Node.js built-ins.

### Quick Start

```typescript
import { launch } from "agent-browser-protocol";

const browser = await launch();
const { client } = browser;

const tabs = await client.tabs.list();
const tabId = tabs[0].id;

await client.tabs.navigate(tabId, { url: "https://example.com" });
await client.tabs.click(tabId, { x: 100, y: 200 });
await client.tabs.type(tabId, { text: "hello world" });

const screenshot = await client.tabs.screenshotBinary(tabId, {
  markup: ["clickable", "typeable"],
});
fs.writeFileSync("screenshot.webp", screenshot);

await browser.close();
```

### Connect to Existing Instance

```typescript
import { ABPClient } from "agent-browser-protocol";

const client = new ABPClient("http://localhost:8222/api/v1");
const tabs = await client.tabs.list();
```

### CLI

```bash
npx agent-browser-protocol                          # launch with defaults
npx agent-browser-protocol --port 9222              # custom port
npx agent-browser-protocol --headless               # headless mode
npx agent-browser-protocol --verbose                # pipe browser output to stderr
npx agent-browser-protocol --session-dir ./session   # persist session data
npx agent-browser-protocol --min-wait 500           # pre-network settlement wait (ms)
npx agent-browser-protocol --tracking-timeout 2000  # request tracking timeout (ms)
npx agent-browser-protocol --post-settle 1000       # post-network settle time (ms)
npx agent-browser-protocol --zoom 1.5              # zoom factor
npx agent-browser-protocol --config-file ./abp.json # config file path
npx agent-browser-protocol --disable-pause          # disable execution control
npx agent-browser-protocol -- --disable-gpu          # pass Chrome flags
```

### SDK Reference

The SDK mirrors the REST API 1:1:

| SDK Method | REST Endpoint |
|-----------|--------------|
| **Browser** | |
| `client.browser.status()` | `GET /browser/status` |
| `client.browser.sessionData()` | `GET /browser/session-data` |
| `client.browser.shutdown()` | `POST /browser/shutdown` |
| `client.browser.inputMode()` | `GET /browser/input-mode` |
| `client.browser.setInputMode({ input_mode })` | `POST /browser/input-mode` |
| `client.browser.cdpModeEnter({ port? })` | `POST /browser/cdp-mode/enter` |
| `client.browser.cdpModeExit()` | `POST /browser/cdp-mode/exit` |
| **Tabs** | |
| `client.tabs.list()` | `GET /tabs` |
| `client.tabs.get(id)` | `GET /tabs/{id}` |
| `client.tabs.create({ url })` | `POST /tabs` |
| `client.tabs.close(id)` | `DELETE /tabs/{id}` |
| `client.tabs.activate(id)` | `POST /tabs/{id}/activate` |
| `client.tabs.stop(id)` | `POST /tabs/{id}/stop` |
| **Navigation** | |
| `client.tabs.navigate(id, { url })` | `POST /tabs/{id}/navigate` |
| `client.tabs.reload(id)` | `POST /tabs/{id}/reload` |
| `client.tabs.back(id)` | `POST /tabs/{id}/back` |
| `client.tabs.forward(id)` | `POST /tabs/{id}/forward` |
| **Input** | |
| `client.tabs.click(id, { x, y })` | `POST /tabs/{id}/click` |
| `client.tabs.move(id, { x, y })` | `POST /tabs/{id}/move` |
| `client.tabs.drag(id, { start_x, start_y, end_x, end_y })` | `POST /tabs/{id}/drag` |
| `client.tabs.type(id, { text })` | `POST /tabs/{id}/type` |
| `client.tabs.keyPress(id, { key })` | `POST /tabs/{id}/keyboard/press` |
| `client.tabs.keyDown(id, { key })` | `POST /tabs/{id}/keyboard/down` |
| `client.tabs.keyUp(id, { key })` | `POST /tabs/{id}/keyboard/up` |
| `client.tabs.scroll(id, { x, y, delta_y })` | `POST /tabs/{id}/scroll` |
| `client.tabs.slider(id, opts)` | `POST /tabs/{id}/slider` |
| `client.tabs.clearText(id, { x, y })` | `POST /tabs/{id}/clear_text` |
| `client.tabs.batch(id, { actions })` | `POST /tabs/{id}/batch` |
| `client.tabs.waitForNetwork(id)` | `POST /tabs/{id}/wait_for_network` |
| `client.tabs.curl(id, { url, method? })` | `POST /tabs/{id}/curl` |
| **Observation** | |
| `client.tabs.screenshot(id)` | `POST /tabs/{id}/screenshot` |
| `client.tabs.screenshotBinary(id)` | `GET /tabs/{id}/screenshot` |
| `client.tabs.execute(id, { script })` | `POST /tabs/{id}/execute` |
| `client.tabs.text(id)` | `POST /tabs/{id}/text` |
| `client.tabs.wait(id, { ms })` | `POST /tabs/{id}/wait` |
| **Dialogs** | |
| `client.tabs.dialog(id)` | `GET /tabs/{id}/dialog` |
| `client.tabs.dialogAccept(id)` | `POST /tabs/{id}/dialog/accept` |
| `client.tabs.dialogDismiss(id)` | `POST /tabs/{id}/dialog/dismiss` |
| **Execution Control** | |
| `client.tabs.execution(id)` | `GET /tabs/{id}/execution` |
| `client.tabs.setExecution(id, { paused })` | `POST /tabs/{id}/execution` |
| **Downloads** | |
| `client.downloads.list()` | `GET /downloads` |
| `client.downloads.get(id)` | `GET /downloads/{id}` |
| `client.downloads.cancel(id)` | `POST /downloads/{id}/cancel` |
| `client.downloads.content(id)` | `GET /downloads/{id}/content` |
| **File Chooser** | |
| `client.fileChooser.provide(id, opts)` | `POST /file-chooser/{id}` |
| **Permissions** | |
| `client.permissions.list()` | `GET /permissions` |
| `client.permissions.grant(id, opts)` | `POST /permissions/{id}/grant` |
| `client.permissions.deny(id, opts)` | `POST /permissions/{id}/deny` |
| **Select Popup** | |
| `client.selectPopup.respond(id, opts)` | `POST /select/{id}` |
| **History** | |
| `client.history.sessions()` | `GET /history/sessions` |
| `client.history.currentSession()` | `GET /history/sessions/current` |
| `client.history.session(id)` | `GET /history/sessions/{id}` |
| `client.history.exportSession(id)` | `GET /history/sessions/{id}/export` |
| `client.history.actions()` | `GET /history/actions` |
| `client.history.action(id)` | `GET /history/actions/{id}` |
| `client.history.actionScreenshot(id)` | `GET /history/actions/{id}/screenshot` |
| `client.history.deleteActions()` | `DELETE /history/actions` |
| `client.history.events()` | `GET /history/events` |
| `client.history.event(id)` | `GET /history/events/{id}` |
| `client.history.deleteEvents()` | `DELETE /history/events` |
| `client.history.deleteAll()` | `DELETE /history` |
| **Network** | |
| `client.network.query({ tag?, url?, type? })` | `GET /network` |
| `client.network.save({ tag })` | `POST /network/save` |
| `client.network.clear(tag?)` | `DELETE /network` |
| **Console** | |
| `client.console.query({ level?, pattern? })` | `GET /console` |
| `client.console.clear({ tab_id? })` | `DELETE /console` |

---

## 2. MCP Server

ABP exposes 21 MCP tools: `browser_action`, `browser_scroll`, `browser_navigate`, `browser_screenshot`, `browser_tabs`, `browser_javascript`, `browser_text`, `browser_wait`, `browser_dialog`, `browser_downloads`, `browser_files`, `browser_select_picker`, `browser_get_status`, `browser_shutdown`, `browser_slider`, `browser_clear_text`, `respond_to_permission`, `browser_network`, `browser_curl`, `browser_console`, `cdp_mode`.

The browser launches automatically on first tool call at 1280x800 (optimized for LLM vision). Screenshots are served as WebP and scaled to fit context limits.

### Claude Code

```bash
claude mcp add browser -- npx -y agent-browser-protocol --mcp
```

The `--mcp` flag runs ABP as a stdio MCP proxy — it launches the browser on first tool call and forwards JSON-RPC to the embedded MCP server.

### Codex CLI

```bash
codex mcp add browser -- npx -y agent-browser-protocol --mcp
```

### Claude Desktop

Let Claude Desktop launch the server over stdio — no port to coordinate. Add to `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "browser": {
      "command": "npx",
      "args": ["-y", "agent-browser-protocol", "--mcp"]
    }
  }
}
```

Restart Claude Desktop after editing the config.

> **macOS:** Claude Desktop launched from Finder doesn't inherit your shell `PATH`, so a bare `"npx"` can fail with `spawn npx ENOENT`. Use the absolute path instead — find it with `which npx` (e.g. `/opt/homebrew/bin/npx` on Apple Silicon).

### Any MCP Client (HTTP)

ABP auto-selects a port starting at `15678`, and it can change between runs. For a static client config, pin the port with `--port`:

```bash
npx -y agent-browser-protocol --port 8222
```

Then point your MCP client at `http://localhost:8222/mcp` (streamable HTTP).

### Configuration

| Variable | Description | Default |
|----------|-------------|---------|
| `ABP_PORT` | Port for ABP server | auto from `15678` |
| `ABP_BROWSER_PATH` | Custom binary path | auto-detected |
| `ABP_HEADLESS` | Run headless (`1`/`0`) | `0` |
| `ABP_VERBOSE` | Pipe browser output to stderr (`1`/`0`) | `0` |
| `ABP_MIN_WAIT` | Pre-network settlement wait (ms) | `150` |
| `ABP_TRACKING_TIMEOUT` | Request tracking timeout (ms) | `1000` |
| `ABP_POST_SETTLE` | Post-network settle time (ms) | `350` |
| `ABP_ZOOM` | Default zoom factor | `1.0` |
| `ABP_CONFIG` | Path to ABP JSON config file | none |
| `ABP_DISABLE_PAUSE` | Disable execution control (`1`/`0`) | `0` |
| `ABP_ARGS` | Extra Chrome args, comma-separated (MCP proxy only) | none |

---

## 3. Debug Server

A web UI for inspecting ABP sessions — action history, before/after screenshots, request params, errors.

```bash
npx abp-debug
```

Opens a local web server (default port 8223) that:

- **Auto-connects** to a running ABP instance and reads its session database
- **Live-updates** via SSE when new actions are recorded
- **Displays** action timeline with before/after screenshots, params, results, and errors
- **Controls** ABP lifecycle — start, stop, restart the browser from the UI
- **Proxies** API requests to ABP (so you can test endpoints from the debug UI)

### Options

```bash
npx abp-debug                                # defaults: debug on 8223, ABP on 8222
npx abp-debug --port 9223                    # custom debug server port
npx abp-debug --abp-url http://localhost:9222 # connect to ABP on different port
npx abp-debug --session-dir ./my-session      # session dir for launching ABP
npx abp-debug --abp-binary /path/to/ABP      # custom ABP binary path
```

The debug server reads ABP's SQLite session database directly (read-only) and watches for filesystem changes to push live updates.

---

## Environment Variables

| Variable | Description |
|---------|------------|
| `ABP_PORT` | Port to listen on (default: auto from `15678`) |
| `ABP_HEADLESS=1` | Run without a visible window |
| `ABP_VERBOSE=1` | Pipe browser output to stderr |
| `ABP_BROWSER_PATH` | Path to a custom ABP binary |
| `ABP_SKIP_DOWNLOAD=1` | Skip binary download during install |
| `ABP_MIN_WAIT` | Pre-network settlement wait in ms (default: `150`) |
| `ABP_TRACKING_TIMEOUT` | Request tracking timeout in ms (default: `1000`) |
| `ABP_POST_SETTLE` | Post-network settle time in ms (default: `350`) |
| `ABP_ZOOM` | Default zoom factor (default: `1.0`) |
| `ABP_CONFIG` | Path to ABP JSON config file |
| `ABP_DISABLE_PAUSE=1` | Disable execution control |
| `ABP_ARGS` | Extra Chrome args, comma-separated (MCP proxy only) |

## Platforms

- macOS (arm64, x64)
- Linux (x64)
- Windows (x64)
