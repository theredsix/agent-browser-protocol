# MCP-REST Parity Implementation Plan

## Problem Statement

The MCP interface currently exposes 13 tools while the REST API provides 25+ endpoints with richer parameters and response data. This gap means AI agents using MCP have fewer capabilities and less control than agents using REST directly. We need full parity so the choice of protocol is about transport preference, not capability.

## Current Gap Analysis

### Missing Tools (Priority Order)

| Priority | REST Endpoint | Why It Matters |
|----------|---------------|----------------|
| P0 | `POST /tabs/{id}/keyboard/press` | Keyboard shortcuts (Ctrl+C, Enter, Escape) - critical for form submission and navigation |
| P0 | `POST /tabs/{id}/scroll` | Page scrolling - required for any content below the fold |
| P0 | `POST /tabs/{id}/move` | Mouse hover - needed for tooltips, dropdown menus |
| P1 | `POST /tabs/{id}/activate` | Tab switching - multi-tab workflows |
| P1 | `POST /tabs/{id}/stop` | Stop loading - timeout recovery |
| P1 | `GET /tabs/{id}/dialog` | Check for pending dialogs |
| P1 | `POST /tabs/{id}/dialog/accept` | Handle alert/confirm/prompt |
| P1 | `POST /tabs/{id}/dialog/dismiss` | Dismiss dialogs |
| P2 | `POST /tabs/{id}/keyboard/down` | Hold modifier keys |
| P2 | `POST /tabs/{id}/keyboard/up` | Release modifier keys |
| P2 | `GET /downloads` | List downloads |
| P2 | `GET /downloads/{id}` | Check download status |
| P2 | `POST /downloads/{id}/cancel` | Cancel download |
| P2 | `POST /file-chooser/{id}` | Provide files to file picker |
| P3 | `GET /tabs/{id}/execution` | Get pause state |
| P3 | `POST /tabs/{id}/execution` | Control JS execution |
| P3 | `POST /browser/shutdown` | Graceful shutdown |

### Reduced Parameters (Tools Missing Options)

| Tool | Missing Parameters | Impact |
|------|-------------------|--------|
| `browser_new_tab` | `active`, `index` | Cannot create background tabs or control position |
| `browser_navigate` | `referrer`, `wait_until`, `screenshot` | No referrer spoofing, no wait control, no automatic screenshot |
| `browser_click` | `button`, `click_count`, `modifiers`, `wait_until`, `screenshot` | No right-click, no double-click, no Ctrl+click, no screenshot |
| `browser_type` | `delay_ms`, `wait_until`, `screenshot` | No typing delay, no screenshot |
| `browser_screenshot` | `area`, `cursor`, `full_page` | No viewport vs full-page control, no cursor visibility |
| `browser_execute_javascript` | `await_promise`, `timeout_ms` | No promise handling control |
| `browser_reload` | `ignore_cache` | Cannot force refresh |

### Missing Response Data

The REST API returns rich action responses with:
- `screenshot` - Auto-captured screenshot after action
- `scroll` - Current scroll position
- `events` - Dialogs, navigations, downloads triggered by action
- `timing` - Action timing metrics

MCP tools currently return only the direct result, losing all this contextual information that helps agents understand what happened.

---

## Implementation Phases

Phases are organized by logical cohesion and what fits in a single focused session. Each phase is independent and testable.

### Phase 1: Response Parity + Core Input Tools

**Scope:** Fix response handling, add 3 critical input tools, expand 2 existing tools

**Files modified:** `abp_mcp_handler.cc`, `abp_mcp_handler.h`

#### 1.1 Fix Response Data (Critical)

Update `OnControllerResponse` to preserve the full REST response:

```cpp
void AbpMcpHandler::OnControllerResponse(int request_id,
                                         McpResponseCallback callback,
                                         int status,
                                         const std::string& content_type,
                                         std::string body) {
  base::Value::Dict result;
  base::Value::List content;
  base::Value::Dict text_content;
  text_content.Set("type", "text");

  auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (parsed && parsed->is_dict()) {
    // Return the complete REST response, not just data field
    std::string pretty_json;
    base::JSONWriter::WriteWithOptions(
        *parsed, base::JSONWriter::OPTIONS_PRETTY_PRINT, &pretty_json);
    text_content.Set("text", pretty_json);
  } else {
    text_content.Set("text", body);
  }

  content.Append(std::move(text_content));
  result.Set("content", std::move(content));

  if (status >= 400) {
    result.Set("isError", true);
  }

  SendJsonRpcResult(request_id, base::Value(std::move(result)), std::move(callback));
}
```

#### 1.2 Add `browser_keyboard_press`

```cpp
// Tool definition
{
  "name": "browser_keyboard_press",
  "description": "Press a key or key combination (e.g., Enter, Escape, Ctrl+C)",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string", "description": "Target tab ID"},
      "key": {"type": "string", "description": "Key to press (e.g., Enter, Escape, a, F1)"},
      "modifiers": {
        "type": "array",
        "items": {"type": "string", "enum": ["Shift", "Control", "Alt", "Meta"]},
        "description": "Modifier keys to hold during press"
      }
    },
    "required": ["tab_id", "key"]
  }
}

// Handler - use args.Clone() pattern for automatic parameter forwarding
void AbpMcpHandler::CallBrowserKeyboardPress(const base::Value::Dict& args,
                                              int request_id,
                                              McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id", std::move(callback));
    return;
  }

  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/keyboard/press", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id, std::move(callback)));
}
```

#### 1.3 Add `browser_scroll`

```cpp
{
  "name": "browser_scroll",
  "description": "Scroll the page using mouse wheel",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string"},
      "x": {"type": "number", "description": "X coordinate for scroll position"},
      "y": {"type": "number", "description": "Y coordinate for scroll position"},
      "delta_x": {"type": "number", "description": "Horizontal scroll amount (negative = left)"},
      "delta_y": {"type": "number", "description": "Vertical scroll amount (negative = down)"}
    },
    "required": ["tab_id", "delta_y"]
  }
}
```

#### 1.4 Add `browser_mouse_move`

```cpp
{
  "name": "browser_mouse_move",
  "description": "Move mouse to coordinates (for hover effects)",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string"},
      "x": {"type": "number"},
      "y": {"type": "number"},
      "steps": {"type": "number", "description": "Intermediate steps for smooth movement"}
    },
    "required": ["tab_id", "x", "y"]
  }
}
```

#### 1.5 Expand `browser_click` Parameters

Add to tool definition:
```cpp
"button": {"type": "string", "enum": ["left", "right", "middle"]},
"click_count": {"type": "number", "description": "1=single, 2=double, 3=triple"},
"modifiers": {"type": "array", "items": {"type": "string"}},
"wait_until": { /* wait_until schema */ },
"screenshot": { /* screenshot schema */ }
```

Update handler to use `args.Clone()` pattern.

#### 1.6 Expand `browser_type` Parameters

Add to tool definition:
```cpp
"delay_ms": {"type": "number", "description": "Delay between keystrokes"},
"wait_until": { /* wait_until schema */ },
"screenshot": { /* screenshot schema */ }
```

#### Phase 1 Checklist

- [x] Update `OnControllerResponse` to preserve full response
- [x] Add `browser_keyboard_press` tool definition + handler
- [x] Add `browser_scroll` tool definition + handler
- [x] Add `browser_mouse_move` tool definition + handler
- [x] Expand `browser_click` tool definition + refactor handler to use `args.Clone()`
- [x] Expand `browser_type` tool definition + refactor handler
- [x] Test all new/modified tools with curl

---

### Phase 2: Tab Control + Dialogs

**Scope:** Tab lifecycle tools, dialog handling, parameter expansion for navigation tools

**Files modified:** `abp_mcp_handler.cc`, `abp_mcp_handler.h`

#### 2.1 Add `browser_activate_tab`

```cpp
{
  "name": "browser_activate_tab",
  "description": "Switch to a specific tab",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string", "description": "ID of tab to activate"}
    },
    "required": ["tab_id"]
  }
}
```

Maps to: `POST /tabs/{id}/activate`

#### 2.2 Add `browser_stop_loading`

```cpp
{
  "name": "browser_stop_loading",
  "description": "Stop page loading",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string"}
    },
    "required": ["tab_id"]
  }
}
```

Maps to: `POST /tabs/{id}/stop`

#### 2.3 Add Dialog Tools

```cpp
// browser_get_dialog
{
  "name": "browser_get_dialog",
  "description": "Check if a dialog (alert/confirm/prompt) is pending",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string"}
    },
    "required": ["tab_id"]
  }
}

// browser_accept_dialog
{
  "name": "browser_accept_dialog",
  "description": "Accept (click OK on) a pending dialog",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string"},
      "prompt_text": {"type": "string", "description": "Text to enter for prompt dialogs"}
    },
    "required": ["tab_id"]
  }
}

// browser_dismiss_dialog
{
  "name": "browser_dismiss_dialog",
  "description": "Dismiss (click Cancel on) a pending dialog",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string"}
    },
    "required": ["tab_id"]
  }
}
```

#### 2.4 Expand Navigation Tool Parameters

**browser_new_tab:**
```cpp
"active": {"type": "boolean", "description": "Whether to activate the new tab"},
"index": {"type": "number", "description": "Position in tab strip"}
```

**browser_navigate:**
```cpp
"referrer": {"type": "string", "description": "Referrer URL"},
"wait_until": { /* wait_until schema */ },
"screenshot": { /* screenshot schema */ }
```

**browser_reload:**
```cpp
"ignore_cache": {"type": "boolean", "description": "Force refresh ignoring cache"}
```

#### Phase 2 Checklist

- [x] Add `browser_activate_tab` tool definition + handler
- [x] Add `browser_stop_loading` tool definition + handler
- [x] Add `browser_get_dialog` tool definition + handler
- [x] Add `browser_accept_dialog` tool definition + handler
- [x] Add `browser_dismiss_dialog` tool definition + handler
- [x] Expand `browser_new_tab` parameters + refactor handler
- [x] Expand `browser_navigate` parameters + refactor handler
- [x] Expand `browser_reload` parameters + refactor handler
- [x] Test all new/modified tools

---

### Phase 3: Downloads, Files, Execution Control

**Scope:** All remaining tools and parameter expansions

**Files modified:** `abp_mcp_handler.cc`, `abp_mcp_handler.h`

#### 3.1 Add Download Tools

```cpp
// browser_list_downloads
{
  "name": "browser_list_downloads",
  "description": "List all downloads",
  "inputSchema": {
    "type": "object",
    "properties": {
      "state": {"type": "string", "enum": ["in_progress", "completed", "cancelled", "failed"]},
      "limit": {"type": "number"}
    }
  }
}

// browser_get_download
{
  "name": "browser_get_download",
  "description": "Get download status",
  "inputSchema": {
    "type": "object",
    "properties": {
      "download_id": {"type": "string"}
    },
    "required": ["download_id"]
  }
}

// browser_cancel_download
{
  "name": "browser_cancel_download",
  "description": "Cancel an in-progress download",
  "inputSchema": {
    "type": "object",
    "properties": {
      "download_id": {"type": "string"}
    },
    "required": ["download_id"]
  }
}
```

#### 3.2 Add File Chooser Tool

```cpp
{
  "name": "browser_provide_files",
  "description": "Provide files to a pending file chooser dialog",
  "inputSchema": {
    "type": "object",
    "properties": {
      "chooser_id": {"type": "string", "description": "File chooser ID from event"},
      "files": {"type": "array", "items": {"type": "string"}, "description": "File paths to provide"},
      "path": {"type": "string", "description": "Save path for save dialogs"},
      "cancel": {"type": "boolean", "description": "Cancel the file chooser"}
    },
    "required": ["chooser_id"]
  }
}
```

#### 3.3 Add Keyboard Down/Up Tools

```cpp
// browser_keyboard_down
{
  "name": "browser_keyboard_down",
  "description": "Press and hold a key",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string"},
      "key": {"type": "string"}
    },
    "required": ["tab_id", "key"]
  }
}

// browser_keyboard_up
{
  "name": "browser_keyboard_up",
  "description": "Release a held key",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string"},
      "key": {"type": "string"}
    },
    "required": ["tab_id", "key"]
  }
}
```

#### 3.4 Add Execution Control Tools

```cpp
// browser_get_execution_state
{
  "name": "browser_get_execution_state",
  "description": "Get JavaScript execution state for a tab",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string"}
    },
    "required": ["tab_id"]
  }
}

// browser_set_execution_state
{
  "name": "browser_set_execution_state",
  "description": "Pause or resume JavaScript execution",
  "inputSchema": {
    "type": "object",
    "properties": {
      "tab_id": {"type": "string"},
      "paused": {"type": "boolean"}
    },
    "required": ["tab_id", "paused"]
  }
}
```

#### 3.5 Add Browser Shutdown Tool

```cpp
{
  "name": "browser_shutdown",
  "description": "Gracefully shut down the browser",
  "inputSchema": {
    "type": "object",
    "properties": {
      "timeout_ms": {"type": "number", "description": "Timeout before force quit"}
    }
  }
}
```

#### 3.6 Expand Remaining Tool Parameters

**browser_screenshot:**
```cpp
"area": {"type": "string", "enum": ["none", "viewport"]},
"cursor": {"type": "boolean", "description": "Include virtual cursor"},
"full_page": {"type": "boolean", "description": "Capture full scrollable page"}
```

**browser_execute_javascript:**
```cpp
"await_promise": {"type": "boolean", "description": "Wait for promise resolution"},
"timeout_ms": {"type": "number", "description": "Timeout for promise resolution"}
```

#### Phase 3 Checklist

- [x] Add `browser_list_downloads` tool definition + handler
- [x] Add `browser_get_download` tool definition + handler
- [x] Add `browser_cancel_download` tool definition + handler
- [x] Add `browser_provide_files` tool definition + handler
- [x] Add `browser_keyboard_down` tool definition + handler
- [x] Add `browser_keyboard_up` tool definition + handler
- [x] Add `browser_get_execution_state` tool definition + handler
- [x] Add `browser_set_execution_state` tool definition + handler
- [x] Add `browser_shutdown` tool definition + handler
- [x] Expand `browser_screenshot` parameters + refactor handler
- [x] Expand `browser_execute_javascript` parameters + refactor handler
- [x] Test all new/modified tools
- [x] Verify parity: count MCP tools vs REST endpoints (30 tools vs 31 endpoints - parity achieved)

---

## Shared Schemas

### wait_until Schema

```cpp
base::Value::Dict GetWaitUntilSchema() {
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict props;

  base::Value::Dict type_prop;
  type_prop.Set("type", "string");
  type_prop.Set("enum", base::Value::List()
      .Append("immediate")
      .Append("action_complete")
      .Append("time"));
  props.Set("type", std::move(type_prop));

  base::Value::Dict timeout_prop;
  timeout_prop.Set("type", "number");
  timeout_prop.Set("description", "Maximum wait time in milliseconds");
  props.Set("timeout_ms", std::move(timeout_prop));

  base::Value::Dict duration_prop;
  duration_prop.Set("type", "number");
  duration_prop.Set("description", "Fixed wait duration for 'time' type");
  props.Set("duration_ms", std::move(duration_prop));

  schema.Set("properties", std::move(props));
  return schema;
}
```

### screenshot Schema

```cpp
base::Value::Dict GetScreenshotOptionsSchema() {
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict props;

  base::Value::Dict area_prop;
  area_prop.Set("type", "string");
  area_prop.Set("enum", base::Value::List().Append("none").Append("viewport"));
  props.Set("area", std::move(area_prop));

  base::Value::Dict markup_prop;
  markup_prop.Set("type", "string");
  markup_prop.Set("enum", base::Value::List()
      .Append("none")
      .Append("interactive")
      .Append("clickable")
      .Append("typeable")
      .Append("inputs"));
  props.Set("markup", std::move(markup_prop));

  base::Value::Dict cursor_prop;
  cursor_prop.Set("type", "boolean");
  props.Set("cursor", std::move(cursor_prop));

  schema.Set("properties", std::move(props));
  return schema;
}
```

---

## Preventing Future Drift

### Rule: REST-First, MCP-Second

Every new REST endpoint must have a corresponding MCP tool added in the same commit.

### Implementation Pattern: Forward All Arguments

MCP tools should forward all received parameters to the REST handler using `args.Clone()`, not explicitly list each one. This ensures new REST parameters automatically work via MCP without code changes.

```cpp
// Preferred pattern
void CallBrowserClick(const base::Value::Dict& args, ...) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) { /* error */ }

  // Forward all arguments except URL path params
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest("POST", "/api/v1/tabs/" + *tab_id + "/click", body, ...);
}
```

### Future Enhancement: Table-Driven Tool Definitions

Consolidate tool definitions to single source of truth:

```cpp
struct ToolDefinition {
  const char* name;
  const char* description;
  const char* rest_method;
  const char* rest_path_template;  // e.g., "/api/v1/tabs/{tab_id}/click"
};

const ToolDefinition kTools[] = {
  {"browser_click", "Click at coordinates", "POST", "/api/v1/tabs/{tab_id}/click"},
  {"browser_type", "Type text", "POST", "/api/v1/tabs/{tab_id}/type"},
  // ...
};
```

---

## Testing

### Manual Test Script

```bash
# Test keyboard press
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/call",
    "params": {
      "name": "browser_keyboard_press",
      "arguments": {"tab_id": "...", "key": "Enter"}
    }
  }'

# Verify response includes screenshot, scroll, events, timing
```

### Parity Verification

After all phases complete, verify:
1. **Tool count**: MCP tools ≥ REST endpoints
2. **Parameter parity**: Every REST parameter available via MCP
3. **Response parity**: MCP responses include screenshot, scroll, events, timing

---

## Success Criteria

1. **Tool count**: MCP exposes same number of tools as REST endpoints (28 tools)
2. **Parameter parity**: Every REST parameter available via MCP
3. **Response parity**: MCP responses include screenshot, scroll, events, timing
4. **No manual sync**: Adding a REST parameter automatically works via MCP (via `args.Clone()` pattern)
