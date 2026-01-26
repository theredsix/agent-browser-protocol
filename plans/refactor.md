# ABP Refactoring Proposal

This document analyzes the Agent Browser Protocol (ABP) implementation and proposes refactoring opportunities to consolidate data structures, reduce duplication, and improve maintainability.

## Current State Analysis

### File Overview

| File | Lines | Responsibility |
|------|-------|----------------|
| `abp_controller.h/cc` | ~2000+ | Core request handling, CDP client, input dispatch, screenshot capture, execution control, dialog handling |
| `abp_http_server.h/cc` | ~280 | HTTP server, thread marshalling, component lifecycle |
| `abp_mcp_handler.h/cc` | ~1800 | MCP JSON-RPC protocol, tool definitions, REST-to-MCP adapter |
| `abp_action_context.h/cc` | ~400 | Action lifecycle: resume -> screenshot -> action -> wait -> pause -> screenshot -> response |
| `abp_history_controller.h/cc` | ~500 | Session/action/event persistence, history REST endpoints |
| `abp_history_database.h/cc` | ~400 | SQLite storage layer |
| `abp_event_collector.h/cc` | ~150 | CDP event capture during actions |
| `abp_event_observer.h/cc` | ~200 | Tab/navigation event forwarding to history |
| `abp_download_observer.h/cc` | ~200 | Download tracking |
| `abp_config.h/cc` | ~180 | Configuration loading |
| `abp_switches.h/cc` | ~30 | Command-line flags |

### Key Data Structures

**Response Callbacks (duplicated definition):**
```cpp
// In abp_controller.h
using ResponseCallback = base::OnceCallback<void(int status,
                                                  const std::string& content_type,
                                                  std::string body)>;

// In abp_action_context.h (same signature, redefined)
using ResponseCallback = base::OnceCallback<void(int status,
                                                  const std::string& content_type,
                                                  std::string body)>;

// In abp_mcp_handler.h (same signature, different name)
using McpResponseCallback = base::OnceCallback<void(int status,
                                                     const std::string& content_type,
                                                     std::string body)>;

// In abp_history_controller.h (same signature, different name)
using HistoryResponseCallback = base::OnceCallback<void(int status,
                                                        const std::string& content_type,
                                                        std::string body)>;
```

**Action Context (duplicated concept):**
```cpp
// In abp_controller.h - legacy struct, still used by some handlers
struct ActionContext {
  std::string tab_id;
  std::string action_type;
  base::Value::Dict params;
  int64_t start_time = 0;
  std::string screenshot_before_path;
};

// In abp_action_context.h - newer RefCounted class with full lifecycle
class AbpActionContext : public base::RefCounted<AbpActionContext> {
  // Similar fields plus more lifecycle state
};
```

**Per-Tab State Maps (scattered across AbpController):**
```cpp
std::map<std::string, std::unique_ptr<AbpCdpClient>> cdp_clients_;
std::map<std::string, VirtualCursorState> virtual_cursor_states_;
std::map<std::string, ExecutionState> execution_states_;
std::map<std::string, HeldKeyState> held_keys_state_;
std::map<std::string, std::unique_ptr<ActionCompleteWaiter>> action_waiters_;
std::map<std::string, PendingDialog> pending_dialogs_;
```

---

## Identified Issues

### 1. Duplicate Callback Type Definitions

**Problem:** Four identical callback signatures defined with different names.

**Impact:**
- Confusing API surface
- Can't easily share handler functions between components
- Inconsistent naming conventions

### 2. Legacy vs. Modern Action Context

**Problem:** `ActionContext` struct in `abp_controller.h` and `AbpActionContext` class coexist. Some actions use the old struct manually, others use the new `AbpActionContext` flow.

**Impact:**
- Inconsistent action handling patterns
- Code duplication in response building and history recording
- Hard to know which pattern to use for new actions

### 3. Monolithic AbpController

**Problem:** `AbpController` at 2000+ lines handles:
- HTTP request routing
- Tab CRUD operations
- Navigation (back/forward/reload)
- All input types (click, type, scroll, key press)
- Screenshot capture (multiple code paths)
- CDP client management
- Execution control (debugger + virtual time)
- Dialog handling
- Virtual cursor state
- Action wait logic

**Impact:**
- Hard to navigate and understand
- Too many responsibilities
- High coupling between unrelated features
- Difficult to test individual features

### 4. Scattered Tab State

**Problem:** Per-tab state is split across 6 different maps, all keyed by tab ID but managed independently.

**Impact:**
- No single place to clean up when a tab closes
- Easy to have orphaned state
- Hard to reason about tab lifecycle

### 5. MCP Tool Definitions Duplication

**Problem:** `GetToolDefinitions()` in `abp_mcp_handler.cc` is 800+ lines of repetitive schema building code. Each tool definition follows the same pattern but is manually constructed.

**Impact:**
- Very verbose
- Easy to make mistakes
- Hard to keep REST API and MCP tools in sync

### 6. Inconsistent Error Response Format

**Problem:** Some endpoints return `{"error": "message"}`, others return `{"success": false, "error": "CODE"}`, and some include error codes while others don't.

**Impact:**
- Clients can't rely on consistent error structure
- Hard to programmatically distinguish error types

### 7. Two Screenshot Capture Paths

**Problem:** Screenshots can be captured via:
1. `CaptureScreenshotDirect()` - CopyFromSurface to SkBitmap
2. `Page.captureScreenshot` CDP command

Both paths exist with different capabilities (markup overlay only works with CDP path).

**Impact:**
- Unclear which path to use when
- Different output quality/behavior
- Code duplication in encoding and file writing

---

## Proposed Refactorings

### Phase 1: Consolidate Types (Low Risk, High Value)

#### 1.1 Unify Response Callback Type

Create `abp_types.h` with shared type definitions:

```cpp
// chrome/browser/abp/abp_types.h
namespace abp {

// Standard response callback for all ABP components
using ResponseCallback = base::OnceCallback<void(
    int status,
    const std::string& content_type,
    std::string body)>;

}  // namespace abp
```

Update all files to use this single definition. Remove `McpResponseCallback` and `HistoryResponseCallback` aliases.

**Files affected:** abp_controller.h, abp_action_context.h, abp_mcp_handler.h, abp_history_controller.h

#### 1.2 Remove Legacy ActionContext

The newer `AbpActionContext` class provides a complete action lifecycle. Migrate remaining actions to use it and remove the legacy `ActionContext` struct.

**Affected actions to migrate:**
- `CreateTab` (currently manual history recording)
- `CloseTab` (currently manual)
- Any other actions still using the old struct

**Rationale:** `AbpActionContext` handles resume/pause, before/after screenshots, event capture, and response envelope consistently. Manual implementations are bug-prone.

### Phase 2: Consolidate Tab State (Medium Risk, Medium Value)

#### 2.1 Create TabState Container

Introduce a single struct to hold all per-tab state:

```cpp
// In abp_controller.h or new abp_tab_state.h
struct TabState {
  std::unique_ptr<AbpCdpClient> cdp_client;
  VirtualCursorState cursor;
  ExecutionState execution;
  HeldKeyState held_keys;
  std::optional<PendingDialog> pending_dialog;
  std::unique_ptr<ActionCompleteWaiter> action_waiter;

  // Lifecycle
  bool IsIdle() const;
  void Reset();
};

// Replace 6 maps with one
std::map<std::string, TabState> tab_states_;
```

**Benefits:**
- Single cleanup point when tab closes
- Clear what state exists per tab
- Easier to add new per-tab state

#### 2.2 Add Tab Close Observer

Register as a WebContentsObserver or TabStripModelObserver to clean up `tab_states_[tab_id]` when tabs close.

### Phase 3: Extract Subsystems from AbpController (Medium Risk, High Value)

Break `AbpController` into focused components:

#### 3.1 Extract AbpInputDispatcher

Move all input-related methods:
- `Click()`, `Type()`, `Move()`, `Scroll()`
- `KeyPress()`, `KeyDown()`, `KeyUp()`
- `held_keys_state_` management
- `virtual_cursor_states_` management
- `UpdateVirtualCursorState()`

New class: `AbpInputDispatcher`

#### 3.2 Extract AbpScreenshotCapture

Move all screenshot logic:
- `Screenshot()`, `BinaryScreenshot()`
- `CaptureScreenshotDirect()`, `CaptureScreenshotForHistory()`
- `CaptureScreenshotBase64()`
- Markup injection logic
- Cursor rendering for screenshots

New class: `AbpScreenshotCapture`

#### 3.3 Extract AbpExecutionControl

Move execution control logic:
- `ResumeExecution()`, `PauseExecution()`
- `EnableExecutionControl()`
- `GetExecutionState()`, `SetExecutionState()`
- `execution_states_` map
- Virtual time management

New class: `AbpExecutionControl`

#### 3.4 Keep AbpController as Facade

After extraction, `AbpController` becomes a thin routing layer that:
- Parses HTTP paths and routes to appropriate handler
- Owns the extracted subsystems
- Provides `FindWebContents()`, `GetOrCreateCdpClient()` utilities
- Delegates to subsystems for actual work

### Phase 4: Simplify MCP Tool Definitions (Low Risk, Medium Value)

#### 4.1 Create Tool Definition Builder

Replace verbose manual JSON building with a declarative approach:

```cpp
// Tool definition builder
ToolBuilder("browser_click")
    .Description("Click at coordinates on the page")
    .RequiredString("tab_id", "Target tab ID")
    .RequiredNumber("x", "X coordinate")
    .RequiredNumber("y", "Y coordinate")
    .OptionalString("button", "Mouse button: left, right, middle")
    .OptionalNumber("click_count", "1=single, 2=double, 3=triple click")
    .OptionalArray("modifiers", "Modifier keys", StringEnum({"Shift", "Control", "Alt", "Meta"}))
    .Build();
```

This cuts `GetToolDefinitions()` from 800 lines to ~200 lines.

#### 4.2 Share Schema with REST API Docs

Consider generating tool definitions from a shared source (JSON schema file or constexpr data) that could also generate API documentation.

### Phase 5: Standardize Error Responses (Low Risk, Low Value)

#### 5.1 Consistent Error Format

Standardize all error responses to:

```json
{
  "success": false,
  "error": {
    "code": "TAB_NOT_FOUND",
    "message": "Tab with ID abc123 not found"
  }
}
```

#### 5.2 Error Code Enum

Define error codes as an enum with string conversion:

```cpp
enum class AbpError {
  kTabNotFound,
  kInvalidRequest,
  kCdpError,
  kTimeout,
  // ...
};

std::string ErrorToCode(AbpError error);
std::string ErrorToMessage(AbpError error, const std::string& context);
```

---

## Suggested Order of Changes

1. **Phase 1.1** - Unify callback types (1-2 hours)
   - Creates `abp_types.h`
   - Mechanical find-replace across files
   - No behavior change

2. **Phase 2.1** - Consolidate tab state (2-4 hours)
   - Create `TabState` struct
   - Migrate maps one at a time
   - Add tab close cleanup

3. **Phase 1.2** - Remove legacy ActionContext (4-8 hours)
   - Audit all actions using old pattern
   - Migrate to `AbpActionContext`
   - Remove `ActionContext` struct

4. **Phase 4.1** - Tool definition builder (4-6 hours)
   - Create builder helper
   - Migrate tool definitions
   - Significant code reduction

5. **Phase 3.x** - Extract subsystems (8-16 hours total)
   - Do one at a time
   - InputDispatcher first (most self-contained)
   - Then ScreenshotCapture
   - Then ExecutionControl
   - Each extraction can be its own PR

6. **Phase 5** - Error standardization (2-4 hours)
   - Can be done anytime
   - Lower priority

---

## What NOT to Refactor (Yet)

- **CDP client architecture**: Works well, no changes needed
- **Threading model**: HTTP/IO thread split is correct
- **History database schema**: Stable and working
- **MCP session management**: Simple and sufficient

---

## Cost-Benefit Analysis

| Refactoring | Effort | Risk | Value | Priority |
|-------------|--------|------|-------|----------|
| Unify callback types | Low | Low | Medium | High |
| Consolidate tab state | Medium | Medium | Medium | Medium |
| Remove legacy ActionContext | Medium | Medium | High | High |
| Tool definition builder | Medium | Low | Medium | Medium |
| Extract InputDispatcher | High | Medium | High | Medium |
| Extract ScreenshotCapture | High | Medium | Medium | Low |
| Extract ExecutionControl | Medium | Medium | Medium | Low |
| Error standardization | Low | Low | Low | Low |

**Recommended starting point:** Phase 1.1 (unify types) and Phase 1.2 (remove legacy ActionContext). These provide the best return on investment with lowest risk.
