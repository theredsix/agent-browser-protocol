# Agent Browser Protocol - Embedded MCP Server Specification

## Overview

The ABP MCP Server is embedded directly in the Chromium browser, exposing browser control capabilities through the Model Context Protocol (MCP). This eliminates the need for a separate Node.js bridge process - starting Chrome with `--enable-abp` provides both REST API and MCP protocol on the same port (8222).

## Implementation Status

**Location:** `chrome/browser/abp/`

The MCP server is implemented in C++ as part of `AbpHttpServer`, handling MCP Streamable HTTP transport alongside the existing REST API.

### Implemented Tools (14 total)

| Tool | Description | REST Endpoint |
|------|-------------|---------------|
| `browser_get_status` | Get browser status | `GET /browser/status` |
| `browser_get_info` | Get browser info | `GET /browser/status` |
| `browser_list_tabs` | List all tabs | `GET /tabs` |
| `browser_new_tab` | Create new tab | `POST /tabs` |
| `browser_close_tab` | Close a tab | `DELETE /tabs/{id}` |
| `browser_get_tab_info` | Get tab details | `GET /tabs/{id}` |
| `browser_navigate` | Navigate to URL | `POST /tabs/{id}/navigate` |
| `browser_go_back` | Go back in history | `POST /tabs/{id}/back` |
| `browser_go_forward` | Go forward in history | `POST /tabs/{id}/forward` |
| `browser_reload` | Reload page | `POST /tabs/{id}/reload` |
| `browser_click` | Click at coordinates | `POST /tabs/{id}/click` |
| `browser_type` | Type text | `POST /tabs/{id}/type` |
| `browser_screenshot` | Take screenshot | `POST /tabs/{id}/screenshot` |
| `browser_execute_javascript` | Execute JavaScript | `POST /tabs/{id}/execute` |

---

## Architecture

### Embedded Design (New)

```
┌─────────────────────────────────────────────────────────────┐
│                    AI Agent / LLM                           │
└─────────────────────────┬───────────────────────────────────┘
                          │ MCP Streamable HTTP (POST/SSE)
                          │ or REST API (GET/POST/DELETE)
                          ▼
┌─────────────────────────────────────────────────────────────┐
│  AbpHttpServer (IO thread)                     Port 8222    │
│  ├── /api/v1/*           → REST API (existing)              │
│  └── /mcp                → MCP Streamable HTTP (new)        │
└─────────────────────────┬───────────────────────────────────┘
                          │ PostTask to UI thread
                          ▼
┌─────────────────────────────────────────────────────────────┐
│  AbpController (UI thread)                                  │
│  - Direct access to Browser, TabStripModel                  │
│  - Uses DevToolsAgentHost for CDP commands                  │
└─────────────────────────────────────────────────────────────┘
```

### Benefits of Embedded Design

1. **Single Process**: No Node.js dependency, no IPC overhead
2. **Single Port**: Both REST and MCP on localhost:8222
3. **Unified Auth**: Same `--abp-auth-token` for both protocols
4. **Lower Latency**: Direct function calls instead of HTTP round-trips
5. **Simpler Deployment**: Just start Chrome with `--enable-abp`

---

## MCP Streamable HTTP Transport

ABP implements the MCP Streamable HTTP transport (protocol version 2025-03-26) with a single endpoint at `/mcp`.

### Endpoint Structure

| Method | Path | Description |
|--------|------|-------------|
| POST | `/mcp` | Send JSON-RPC messages (requests, notifications, responses) |
| GET | `/mcp` | Open SSE stream for server-initiated messages |
| DELETE | `/mcp` | Terminate session (optional) |

### Request Headers

| Header | Required | Description |
|--------|----------|-------------|
| `Content-Type` | Yes | `application/json` |
| `Accept` | Yes | `application/json, text/event-stream` |
| `Authorization` | If auth enabled | `Bearer <token>` |
| `Mcp-Session-Id` | After init | Session ID from InitializeResult |
| `MCP-Protocol-Version` | Yes | `2025-03-26` |

### Response Modes

The server responds to POST requests in one of two ways:

**1. Single JSON Response** (`Content-Type: application/json`)
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": { ... }
}
```

**2. SSE Stream** (`Content-Type: text/event-stream`)
```
data: {"jsonrpc":"2.0","id":1,"result":{...}}

```

For ABP, most tool calls return single JSON responses. SSE streaming is used for:
- Long-running operations (navigation with wait conditions)
- Server-initiated notifications (browser events)

### Session Management

1. Client sends `initialize` request without session ID
2. Server responds with `Mcp-Session-Id` header
3. Client includes session ID in all subsequent requests
4. Session expires after 30 minutes of inactivity (configurable)
5. HTTP 404 response indicates session expired - client must reinitialize

---

## MCP Message Flow

### Initialization

```
Client                                  Server (ABP)
  │                                        │
  │  POST /mcp                             │
  │  Content-Type: application/json        │
  │  Accept: application/json, text/event-stream
  │  {"jsonrpc":"2.0","id":1,"method":"initialize","params":{
  │    "protocolVersion":"2025-03-26",
  │    "clientInfo":{"name":"claude","version":"1.0"},
  │    "capabilities":{}
  │  }}
  │ ─────────────────────────────────────► │
  │                                        │
  │  HTTP 200 OK                           │
  │  Content-Type: application/json        │
  │  Mcp-Session-Id: abc123...             │
  │  {"jsonrpc":"2.0","id":1,"result":{
  │    "protocolVersion":"2025-03-26",
  │    "serverInfo":{"name":"abp-browser","version":"1.0.0"},
  │    "capabilities":{"tools":{}}
  │  }}
  │ ◄───────────────────────────────────── │
  │                                        │
  │  POST /mcp                             │
  │  Mcp-Session-Id: abc123...             │
  │  {"jsonrpc":"2.0","method":"notifications/initialized"}
  │ ─────────────────────────────────────► │
  │                                        │
  │  HTTP 202 Accepted                     │
  │ ◄───────────────────────────────────── │
```

### Tool Call (Simple)

```
Client                                  Server (ABP)
  │                                        │
  │  POST /mcp                             │
  │  Mcp-Session-Id: abc123...             │
  │  {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{
  │    "name":"browser_list_tabs",
  │    "arguments":{}
  │  }}
  │ ─────────────────────────────────────► │
  │                                        │
  │  HTTP 200 OK                           │
  │  Content-Type: application/json        │
  │  {"jsonrpc":"2.0","id":2,"result":{
  │    "content":[{"type":"text","text":"[{\"id\":\"...\"}]"}]
  │  }}
  │ ◄───────────────────────────────────── │
```

### Tool Call with SSE (Long-running)

For operations like navigation with `wait_until: network_idle`:

```
Client                                  Server (ABP)
  │                                        │
  │  POST /mcp                             │
  │  {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{
  │    "name":"browser_navigate",
  │    "arguments":{"tab_id":"...","url":"https://example.com",
  │      "wait_until":{"type":"network_idle"}}
  │  }}
  │ ─────────────────────────────────────► │
  │                                        │
  │  HTTP 200 OK                           │
  │  Content-Type: text/event-stream       │
  │                                        │
  │  data: {"jsonrpc":"2.0","method":"notifications/progress",
  │         "params":{"token":3,"value":{"kind":"report","message":"Loading..."}}}
  │ ◄───────────────────────────────────── │
  │                                        │
  │  data: {"jsonrpc":"2.0","id":3,"result":{
  │         "content":[{"type":"text","text":"{\"url\":\"...\"}"}]}}
  │ ◄───────────────────────────────────── │
  │                                        │
  │  [Connection closed by server]         │
```

### Server-Initiated Messages (GET Stream)

For browser events (dialogs, downloads, etc.):

```
Client                                  Server (ABP)
  │                                        │
  │  GET /mcp                              │
  │  Accept: text/event-stream             │
  │  Mcp-Session-Id: abc123...             │
  │ ─────────────────────────────────────► │
  │                                        │
  │  HTTP 200 OK                           │
  │  Content-Type: text/event-stream       │
  │                                        │
  │  [Connection held open]                │
  │                                        │
  │  data: {"jsonrpc":"2.0","method":"notifications/browser/dialog",
  │         "params":{"type":"alert","message":"Hello!"}}
  │ ◄───────────────────────────────────── │
  │                                        │
  │  data: {"jsonrpc":"2.0","method":"notifications/browser/download",
  │         "params":{"id":"dl_1","state":"completed"}}
  │ ◄───────────────────────────────────── │
```

---

## Implementation Details

### File Structure

```
chrome/browser/abp/
├── BUILD.gn                    # Add mcp files
├── abp_http_server.h/cc        # Add MCP routing
├── abp_mcp_handler.h/cc        # NEW: MCP protocol handler
├── abp_mcp_session.h/cc        # NEW: Session state management
└── abp_mcp_tools.h/cc          # NEW: Tool definitions and dispatch
```

### AbpMcpHandler Class

```cpp
// abp_mcp_handler.h
namespace abp {

class AbpMcpHandler {
 public:
  explicit AbpMcpHandler(AbpController* controller);

  // Handle POST to /mcp - returns response via callback
  // Callback receives: (status_code, content_type, body)
  // For SSE, callback is called multiple times with event data
  void HandlePost(const std::string& body,
                  const std::string& session_id,
                  ResponseCallback callback);

  // Handle GET to /mcp - opens SSE stream
  // Returns session_id for new sessions
  void HandleGet(const std::string& session_id,
                 SSECallback sse_callback);

  // Handle DELETE to /mcp - terminates session
  void HandleDelete(const std::string& session_id,
                    ResponseCallback callback);

 private:
  // JSON-RPC dispatch
  void HandleInitialize(const base::Value::Dict& params,
                        ResponseCallback callback);
  void HandleToolsList(ResponseCallback callback);
  void HandleToolsCall(const base::Value::Dict& params,
                       ResponseCallback callback);
  void HandleResourcesList(ResponseCallback callback);
  void HandleResourcesRead(const base::Value::Dict& params,
                           ResponseCallback callback);

  // Session management
  std::string CreateSession();
  AbpMcpSession* GetSession(const std::string& session_id);
  void CleanupExpiredSessions();

  AbpController* controller_;  // Not owned
  std::map<std::string, std::unique_ptr<AbpMcpSession>> sessions_;
};

}  // namespace abp
```

### Routing in AbpHttpServer

```cpp
// In abp_http_server.cc HandleRequestOnUI()

void AbpHttpServer::HandleRequestOnUI(int connection_id,
                                      std::string method,
                                      std::string path,
                                      std::string body) {
  // ... existing code ...

  // Route MCP requests
  if (path == "/mcp" || base::StartsWith(path, "/mcp?")) {
    std::string session_id = ExtractSessionId(headers);

    if (method == "POST") {
      mcp_handler_->HandlePost(body, session_id, std::move(callback));
    } else if (method == "GET") {
      mcp_handler_->HandleGet(session_id, std::move(sse_callback));
    } else if (method == "DELETE") {
      mcp_handler_->HandleDelete(session_id, std::move(callback));
    } else {
      std::move(callback).Run(405, "application/json",
                              R"({"error":"Method not allowed"})");
    }
    return;
  }

  // ... existing REST API routing ...
}
```

### Tool Dispatch

The MCP handler translates tool calls directly to AbpController methods, avoiding HTTP round-trips:

```cpp
void AbpMcpHandler::HandleToolsCall(const base::Value::Dict& params,
                                    ResponseCallback callback) {
  std::string* tool_name = params.FindString("name");
  const base::Value::Dict* args = params.FindDict("arguments");

  if (*tool_name == "browser_list_tabs") {
    // Direct call to controller
    base::Value::List tabs = controller_->GetTabs();
    SendToolResult(std::move(callback), TabsToJson(tabs));

  } else if (*tool_name == "browser_navigate") {
    std::string* tab_id = args->FindString("tab_id");
    std::string* url = args->FindString("url");
    controller_->Navigate(*tab_id, *url,
        base::BindOnce(&AbpMcpHandler::OnNavigateComplete,
                       weak_factory_.GetWeakPtr(),
                       std::move(callback)));

  } else if (*tool_name == "browser_screenshot") {
    // ... etc
  }
}
```

### SSE Connection Management

For server-initiated notifications, maintain a list of active SSE connections per session:

```cpp
class AbpMcpSession {
 public:
  void AddSSEConnection(int connection_id, SSECallback callback);
  void RemoveSSEConnection(int connection_id);
  void BroadcastNotification(const base::Value::Dict& notification);

 private:
  std::string session_id_;
  base::Time created_at_;
  base::Time last_activity_;
  std::map<int, SSECallback> sse_connections_;
};
```

---

## Authentication

MCP uses the same authentication as the REST API:

```bash
# Start with auth token
./chrome --enable-abp --abp-auth-token=secret123

# MCP requests include Bearer token
curl -X POST http://localhost:8222/mcp \
  -H "Authorization: Bearer secret123" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize",...}'
```

The `Authorization` header is validated before processing any MCP request. Invalid/missing tokens return HTTP 401.

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

### Environment Variables

For clients connecting to ABP MCP:

```bash
ABP_MCP_URL=http://localhost:8222/mcp
ABP_AUTH_TOKEN=secret123
```

---

## Client Configuration

### Claude Desktop

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

### Programmatic Client (Python)

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
        self.session_id = response.headers.get("Mcp-Session-Id")
        return response.json()

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

## MCP Tools Reference

### Browser Management

#### `browser_get_status`
Get browser initialization status.

**Parameters:** None

**Returns:**
```json
{
  "ready": true,
  "abp_version": "1.0.0"
}
```

#### `browser_get_info`
Get browser version and status information.

**Parameters:** None

**Returns:**
```json
{
  "browser": "ABP-Chromium",
  "version": "120.0.0.0",
  "abp_version": "1.0.0"
}
```

---

### Tab Management

#### `browser_list_tabs`
List all open browser tabs.

**Parameters:** None

**Returns:**
```json
{
  "tabs": [
    {
      "id": "ABC123...",
      "url": "https://example.com",
      "title": "Example Domain",
      "active": true
    }
  ]
}
```

#### `browser_new_tab`
Create a new browser tab.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `url` | string | No | URL to navigate to (default: blank) |

**Returns:**
```json
{
  "id": "XYZ789...",
  "url": "https://example.com"
}
```

#### `browser_close_tab`
Close a browser tab.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | Yes | ID of tab to close |

#### `browser_get_tab_info`
Get detailed information about a tab.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | Yes | ID of tab |

**Returns:**
```json
{
  "id": "ABC123...",
  "url": "https://example.com",
  "title": "Example Domain",
  "loading": false,
  "can_go_back": true,
  "can_go_forward": false
}
```

---

### Navigation

#### `browser_navigate`
Navigate to a URL.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | Yes | Target tab ID |
| `url` | string | Yes | URL to navigate to |

**Returns:**
```json
{
  "url": "https://example.com",
  "title": "Example Domain"
}
```

#### `browser_go_back`
Navigate back in history.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | Yes | Target tab ID |

#### `browser_go_forward`
Navigate forward in history.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | Yes | Target tab ID |

#### `browser_reload`
Reload the current page.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | Yes | Target tab ID |

---

### Mouse Actions

#### `browser_click`
Click at coordinates.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | Yes | Target tab ID |
| `x` | number | Yes | X coordinate |
| `y` | number | Yes | Y coordinate |

---

### Keyboard Actions

#### `browser_type`
Type text at current focus position.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | Yes | Target tab ID |
| `text` | string | Yes | Text to type |

---

### Screenshots

#### `browser_screenshot`
Take a screenshot of the page.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | Yes | Target tab ID |
| `format` | string | No | `png`, `jpeg`, `webp` (default: `webp`) |
| `quality` | number | No | Image quality 1-100 for jpeg/webp |
| `markup` | string | No | Element markup overlay type |

**Markup values:**
- `none` - No element markup
- `interactive` - All interactive elements (clickable + typeable)
- `clickable` - Buttons, links, clickable elements
- `typeable` - Text inputs, textareas, contenteditable
- `inputs` - All form inputs

**Returns:**
```json
{
  "data": "base64-encoded-image-data",
  "format": "webp",
  "width": 1920,
  "height": 1080,
  "marked_elements": [
    {
      "index": 0,
      "type": "button",
      "bounds": {"x": 100, "y": 200, "width": 80, "height": 32},
      "center": {"x": 140, "y": 216},
      "text": "Submit"
    }
  ]
}
```

---

### JavaScript Execution

#### `browser_execute_javascript`
Execute JavaScript in the page context.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | Yes | Target tab ID |
| `expression` | string | Yes | JavaScript expression to evaluate |

**Returns:**
```json
{
  "value": 42,
  "type": "number"
}
```

---

## MCP Resources

The MCP server exposes resources for reading browser state.

### `browser://status`
Browser initialization and readiness status.

### `browser://tabs`
List of all open browser tabs.

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
| -32601 | METHOD_NOT_FOUND | Unknown method |
| -32602 | INVALID_PARAMS | Invalid parameters |
| -32000 | TAB_NOT_FOUND | Tab does not exist |
| -32001 | NAVIGATION_FAILED | Navigation failed |
| -32002 | TIMEOUT | Operation timed out |
| -32003 | EVALUATION_ERROR | JavaScript error |

### HTTP Status Codes

| Code | Meaning |
|------|---------|
| 200 | Success (with JSON or SSE body) |
| 202 | Accepted (for notifications/responses) |
| 400 | Bad request (invalid JSON-RPC) |
| 401 | Unauthorized (missing/invalid auth token) |
| 404 | Session expired |
| 405 | Method not allowed |

---

## Migration from Node.js MCP Server

The Node.js MCP server (`tools/abp-mcp-server/`) is deprecated. To migrate:

### Before (Node.js bridge)

```json
{
  "mcpServers": {
    "browser": {
      "command": "node",
      "args": ["/path/to/abp-mcp-server/dist/index.js"],
      "env": {
        "ABP_URL": "http://localhost:8222"
      }
    }
  }
}
```

### After (Embedded)

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

The tool names and parameters are identical - no changes needed to agent code.

---

## Implementation Phases

### Phase 1: Basic MCP (MVP)

- [ ] Add `/mcp` endpoint routing in `abp_http_server.cc`
- [ ] Implement `AbpMcpHandler` with JSON-RPC parsing
- [ ] Support `initialize`, `tools/list`, `tools/call`
- [ ] Map tools to direct `AbpController` calls
- [ ] Single JSON response mode only
- [ ] No session management (stateless)

### Phase 2: Sessions and SSE

- [ ] Add `AbpMcpSession` class
- [ ] Implement session ID generation and tracking
- [ ] Add session timeout cleanup
- [ ] Support SSE response mode for tool calls
- [ ] Implement GET stream for server notifications

### Phase 3: Resources and Notifications

- [ ] Add `resources/list` and `resources/read`
- [ ] Implement browser event notifications (dialogs, downloads)
- [ ] Add progress notifications for long operations

### Phase 4: Polish

- [ ] Add `MCP-Protocol-Version` header validation
- [ ] Implement resumability with event IDs
- [ ] Add metrics and logging
- [ ] Remove deprecated Node.js MCP server

---

## Related Documents

- [agent-browser-protocol.md](./agent-browser-protocol.md) - Core ABP architecture
- [API.md](./API.md) - REST API specification
- [MCP Specification](https://modelcontextprotocol.io/specification/2025-03-26/basic/transports) - Official MCP transport spec
