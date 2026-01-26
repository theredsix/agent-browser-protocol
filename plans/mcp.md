# Agent Browser Protocol - Embedded MCP Server Specification

## Overview

The ABP MCP Server is embedded directly in the Chromium browser, exposing browser control capabilities through the Model Context Protocol (MCP). This eliminates the need for a separate bridge process - starting Chrome with `--enable-abp` provides both REST API and MCP protocol on the same port (8222).

**Design Principle:** MCP is a transport layer for the REST API. Every REST endpoint has a corresponding MCP tool with identical parameters and response data. The choice of protocol is about transport preference, not capability.

## Implementation Status

**Location:** `chrome/browser/abp/abp_mcp_handler.cc`

See [mcp_rest_parity.md](./mcp_rest_parity.md) for detailed implementation plan and current progress.

---

## Architecture

```
+-------------------------------------------------------------+
|                    AI Agent / LLM                           |
+---------------------------+---------------------------------+
                            | MCP Streamable HTTP (POST/SSE)
                            | or REST API (GET/POST/DELETE)
                            v
+-------------------------------------------------------------+
|  AbpHttpServer (IO thread)                     Port 8222    |
|  +-- /api/v1/*           -> REST API                        |
|  +-- /mcp                -> MCP Streamable HTTP             |
+---------------------------+---------------------------------+
                            | PostTask to UI thread
                            v
+-------------------------------------------------------------+
|  AbpController (UI thread)                                  |
|  - Direct access to Browser, TabStripModel                  |
|  - Uses DevToolsAgentHost for CDP commands                  |
+-------------------------------------------------------------+
```

### Benefits of Embedded Design

1. **Single Process**: No Node.js dependency, no IPC overhead
2. **Single Port**: Both REST and MCP on localhost:8222
3. **Unified Auth**: Same `--abp-auth-token` for both protocols
4. **Lower Latency**: Direct function calls instead of HTTP round-trips
5. **Simpler Deployment**: Just start Chrome with `--enable-abp`

---

## REST-to-MCP Translation

### Naming Convention

REST endpoints map to MCP tools following this pattern:

| REST Pattern | MCP Tool Name |
|--------------|---------------|
| `GET /tabs` | `browser_list_tabs` |
| `POST /tabs` | `browser_new_tab` |
| `GET /tabs/{id}` | `browser_get_tab_info` |
| `DELETE /tabs/{id}` | `browser_close_tab` |
| `POST /tabs/{id}/navigate` | `browser_navigate` |
| `POST /tabs/{id}/click` | `browser_click` |
| `POST /tabs/{id}/keyboard/press` | `browser_keyboard_press` |
| `GET /browser/status` | `browser_get_status` |
| `POST /browser/shutdown` | `browser_shutdown` |

**Pattern:**
- Resource becomes prefix: `browser_`
- HTTP method + path becomes verb: `list_tabs`, `new_tab`, `close_tab`
- Sub-resources use underscores: `keyboard_press`, `dialog_accept`

### Parameter Mapping

All REST request body parameters become MCP tool arguments. URL path parameters (like `{tab_id}`) become required arguments.

**REST:**
```json
POST /api/v1/tabs/ABC123/click
{
  "x": 100,
  "y": 200,
  "button": "left",
  "click_count": 2
}
```

**MCP:**
```json
{
  "method": "tools/call",
  "params": {
    "name": "browser_click",
    "arguments": {
      "tab_id": "ABC123",
      "x": 100,
      "y": 200,
      "button": "left",
      "click_count": 2
    }
  }
}
```

### Response Mapping

MCP tool results contain the complete REST response in a text content block:

**REST Response:**
```json
{
  "result": {"x": 100, "y": 200, "button": "left"},
  "screenshot": {"data": "...", "width": 1920, "height": 1080},
  "scroll": {"vertical_percent": 25.5, ...},
  "events": [{"type": "navigation", ...}],
  "timing": {"duration_ms": 150}
}
```

**MCP Response:**
```json
{
  "result": {
    "content": [{
      "type": "text",
      "text": "{\"result\": ..., \"screenshot\": ..., \"scroll\": ..., \"events\": ..., \"timing\": ...}"
    }]
  }
}
```

The full response structure is preserved, including screenshots, scroll positions, events, and timing data.

---

## MCP Streamable HTTP Transport

ABP implements the MCP Streamable HTTP transport (protocol version 2025-03-26) with a single endpoint at `/mcp`.

### Endpoint Structure

| Method | Path | Description |
|--------|------|-------------|
| POST | `/mcp` | Send JSON-RPC messages (requests, notifications, responses) |
| GET | `/mcp` | Open SSE stream for server-initiated messages |
| DELETE | `/mcp` | Terminate session |

### Request Headers

| Header | Required | Description |
|--------|----------|-------------|
| `Content-Type` | Yes | `application/json` |
| `Accept` | Yes | `application/json, text/event-stream` |
| `Authorization` | If auth enabled | `Bearer <token>` |
| `Mcp-Session-Id` | After init | Session ID from initialize response |
| `MCP-Protocol-Version` | Yes | `2025-03-26` |

### Session Management

1. Client sends `initialize` request without session ID
2. Server responds with `_mcpSessionId` in response body
3. Client includes session ID in all subsequent requests via `Mcp-Session-Id` header
4. Session expires after 30 minutes of inactivity (configurable via `--abp-mcp-session-timeout`)
5. HTTP 404 response indicates session expired - client must reinitialize

---

## Message Flow

### Initialization

```
Client                                  Server (ABP)
  |                                        |
  |  POST /mcp                             |
  |  Content-Type: application/json        |
  |  Accept: application/json, text/event-stream
  |  {"jsonrpc":"2.0","id":1,"method":"initialize","params":{
  |    "protocolVersion":"2025-03-26",
  |    "clientInfo":{"name":"claude","version":"1.0"},
  |    "capabilities":{}
  |  }}
  | --------------------------------------> |
  |                                        |
  |  HTTP 200 OK                           |
  |  Content-Type: application/json        |
  |  {"jsonrpc":"2.0","id":1,"result":{
  |    "protocolVersion":"2025-03-26",
  |    "serverInfo":{"name":"abp-browser","version":"1.0.0"},
  |    "capabilities":{"tools":{}}
  |  },
  |  "_mcpSessionId":"abc123..."}
  | <-------------------------------------- |
  |                                        |
  |  POST /mcp                             |
  |  Mcp-Session-Id: abc123...             |
  |  {"jsonrpc":"2.0","method":"notifications/initialized"}
  | --------------------------------------> |
  |                                        |
  |  HTTP 202 Accepted                     |
  | <-------------------------------------- |
```

### Tool Call

```
Client                                  Server (ABP)
  |                                        |
  |  POST /mcp                             |
  |  Mcp-Session-Id: abc123...             |
  |  {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{
  |    "name":"browser_click",
  |    "arguments":{"tab_id":"...","x":100,"y":200}
  |  }}
  | --------------------------------------> |
  |                                        |
  |  HTTP 200 OK                           |
  |  Content-Type: application/json        |
  |  {"jsonrpc":"2.0","id":2,"result":{
  |    "content":[{"type":"text","text":"{...}"}]
  |  }}
  | <-------------------------------------- |
```

---

## Configuration

### Command Line Flags

| Flag | Description |
|------|-------------|
| `--enable-abp` | Enable ABP (REST + MCP) |
| `--abp-port=PORT` | Server port (default: 8222) |
| `--abp-auth-token=TOKEN` | Authentication token |
| `--abp-session-dir=PATH` | Session data directory |
| `--abp-mcp-session-timeout=SECONDS` | MCP session timeout (default: 1800) |

### Client Configuration

#### Claude Desktop

Add to `claude_desktop_config.json`:

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

With authentication:

```json
{
  "mcpServers": {
    "browser": {
      "transport": "streamable-http",
      "url": "http://localhost:8222/mcp",
      "headers": {
        "Authorization": "Bearer secret123"
      }
    }
  }
}
```

#### Programmatic Client (Python)

```python
import httpx

class ABPMcpClient:
    def __init__(self, url="http://localhost:8222/mcp", token=None):
        self.url = url
        self.session_id = None
        self.headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
            "MCP-Protocol-Version": "2025-03-26",
        }
        if token:
            self.headers["Authorization"] = f"Bearer {token}"

    def initialize(self):
        response = httpx.post(self.url, headers=self.headers, json={
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-03-26",
                "clientInfo": {"name": "python-client", "version": "1.0"},
                "capabilities": {}
            }
        })
        data = response.json()
        self.session_id = data.get("_mcpSessionId")
        return data

    def call_tool(self, name, arguments=None):
        headers = {**self.headers, "Mcp-Session-Id": self.session_id}
        response = httpx.post(self.url, headers=headers, json={
            "jsonrpc": "2.0",
            "id": 2,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments or {}}
        })
        return response.json()

# Usage
client = ABPMcpClient(token="secret123")
client.initialize()
tabs = client.call_tool("browser_list_tabs")
```

---

## Error Handling

### JSON-RPC Errors

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32000,
    "message": "Tab not found",
    "data": {
      "tab_id": "invalid_id",
      "abp_error_code": "TAB_NOT_FOUND"
    }
  }
}
```

### Error Codes

| MCP Code | ABP Code | Description |
|----------|----------|-------------|
| -32700 | PARSE_ERROR | Invalid JSON |
| -32600 | INVALID_REQUEST | Invalid JSON-RPC |
| -32601 | METHOD_NOT_FOUND | Unknown method or tool |
| -32602 | INVALID_PARAMS | Invalid or missing parameters |
| -32000 | TAB_NOT_FOUND | Tab does not exist |
| -32001 | NAVIGATION_FAILED | Navigation failed |
| -32002 | TIMEOUT | Operation timed out |
| -32003 | EVALUATION_ERROR | JavaScript error |

### HTTP Status Codes

| Code | Meaning |
|------|---------|
| 200 | Success (with JSON body) |
| 202 | Accepted (for notifications) |
| 204 | No content (for DELETE) |
| 400 | Bad request (invalid JSON-RPC) |
| 401 | Unauthorized (missing/invalid auth token) |
| 404 | Session expired |
| 405 | Method not allowed |

---

## Tool Reference

Tools are generated from the REST API specification. See [API.md](./API.md) for complete endpoint documentation. Each REST endpoint has a corresponding MCP tool with the same parameters.

### Quick Reference

**Tab Management:**
- `browser_list_tabs` - List all tabs
- `browser_new_tab` - Create tab
- `browser_close_tab` - Close tab
- `browser_get_tab_info` - Get tab details
- `browser_activate_tab` - Switch to tab
- `browser_stop_loading` - Stop page load

**Navigation:**
- `browser_navigate` - Go to URL
- `browser_go_back` - History back
- `browser_go_forward` - History forward
- `browser_reload` - Reload page

**Mouse:**
- `browser_click` - Click at coordinates
- `browser_mouse_move` - Move mouse
- `browser_scroll` - Scroll page

**Keyboard:**
- `browser_type` - Type text
- `browser_keyboard_press` - Press key/shortcut
- `browser_keyboard_down` - Hold key
- `browser_keyboard_up` - Release key

**Screenshots:**
- `browser_screenshot` - Capture viewport

**JavaScript:**
- `browser_execute_javascript` - Run script

**Dialogs:**
- `browser_get_dialog` - Check for pending dialog
- `browser_accept_dialog` - Accept dialog
- `browser_dismiss_dialog` - Dismiss dialog

**Downloads:**
- `browser_list_downloads` - List downloads
- `browser_get_download` - Get download status
- `browser_cancel_download` - Cancel download

**Files:**
- `browser_provide_files` - Provide files to file chooser

**Execution Control:**
- `browser_get_execution_state` - Get JS pause state
- `browser_set_execution_state` - Pause/resume JS

**Browser:**
- `browser_get_status` - Get browser status
- `browser_shutdown` - Shut down browser

---

## Implementation Details

### File Structure

```
chrome/browser/abp/
+-- BUILD.gn                    # Build configuration
+-- abp_http_server.h/cc        # HTTP server with MCP routing
+-- abp_mcp_handler.h/cc        # MCP protocol handler and tool dispatch
+-- abp_controller.h/cc         # Shared REST/MCP request handling
```

### Adding a New Tool

When adding a new REST endpoint, add the corresponding MCP tool:

1. **Add tool definition** in `GetToolDefinitions()`:
```cpp
{
  base::Value::Dict tool;
  tool.Set("name", "browser_new_tool");
  tool.Set("description", "Description matching REST API");
  // ... input schema matching REST parameters
  tools.Append(std::move(tool));
}
```

2. **Add dispatch case** in `HandleToolsCall()`:
```cpp
} else if (*name == "browser_new_tool") {
  CallBrowserNewTool(*args, request_id, std::move(callback));
}
```

3. **Add handler method** that forwards to REST:
```cpp
void AbpMcpHandler::CallBrowserNewTool(const base::Value::Dict& args,
                                       int request_id,
                                       McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  // ... validation ...

  // Forward all args to REST endpoint
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");  // Remove URL path params

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/new-endpoint", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id, std::move(callback)));
}
```

---

## Related Documents

- [API.md](./API.md) - REST API specification (source of truth)
- [mcp_rest_parity.md](./mcp_rest_parity.md) - Implementation plan for full parity
- [agent-browser-protocol.md](./agent-browser-protocol.md) - Core ABP architecture
- [MCP Specification](https://modelcontextprotocol.io/specification/2025-03-26/basic/transports) - Official MCP transport spec
