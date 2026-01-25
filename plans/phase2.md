# Phase 2: Response Envelope & Event System - COMPLETE

**Status: FULLY IMPLEMENTED**

All core components for Phase 2 are implemented:
- [x] AbpEventCollector class for capturing events during actions
- [x] AbpActionContext for unified action flow
- [x] Screenshot capture to disk for history
- [x] Virtual time integration
- [x] CDP event subscription (Page.enable, Overlay.enable)
- [x] Dialog, download, file chooser event capture
- [x] Navigation and popup event capture

Screenshots are saved to disk for history and returned inline in screenshot endpoint responses. The infrastructure supports the full response envelope.

## Goal

Implement the full response envelope for action endpoints. After this phase, every action returns a screenshot, scroll position, captured events, and timing—enabling agents to see results and react to side effects.

## Components to Implement

### 1. Screenshot in Response

Currently screenshots are saved to disk for history. Add inline base64 screenshot to response.

**Response format:**
```json
{
  "screenshot": {
    "data": "base64-encoded-webp",
    "width": 1920,
    "height": 1080,
    "virtual_time_ms": 1699999999999,
    "markup": "interactive"
  }
}
```

**Implementation:**
- After `CopyFromSurface` completes, encode as WebP
- Base64 encode the WebP data
- Get virtual time from execution state
- Include in response JSON

**Files to modify:**
- `abp_controller.cc` - Modify `CompleteActionWithScreenshot()` to include screenshot data
- Add base64 encoding step after WebP encoding

**Key change:**
```cpp
// Current: saves to file, returns path
// New: also returns base64 data in response

struct ActionResponse {
  base::Value::Dict result;
  std::string screenshot_base64;
  int screenshot_width;
  int screenshot_height;
  double virtual_time_ms;
  std::string markup_mode;
  // ... scroll, events, timing
};
```

---

### 2. Scroll Position in Response

Return current scroll state after each action.

**Response format:**
```json
{
  "scroll": {
    "horizontal_percent": 0,
    "vertical_percent": 25.5,
    "horizontal_px": 0,
    "vertical_px": 1200,
    "page_width": 1920,
    "page_height": 4700,
    "viewport_width": 1920,
    "viewport_height": 1080
  }
}
```

**Implementation:**
- Use CDP `Runtime.evaluate` to get scroll info:
```javascript
({
  scrollX: window.scrollX,
  scrollY: window.scrollY,
  pageWidth: document.documentElement.scrollWidth,
  pageHeight: document.documentElement.scrollHeight,
  viewportWidth: window.innerWidth,
  viewportHeight: window.innerHeight
})
```
- Calculate percentages from raw values
- Include in response envelope

**Files to modify:**
- `abp_controller.cc` - Add `GetScrollPosition()` method
- Call during `CompleteActionWithScreenshot()`

---

### 3. Timing in Response

Return action timing metrics.

**Response format:**
```json
{
  "timing": {
    "action_started_ms": 1699999999000,
    "action_completed_ms": 1699999999050,
    "wait_completed_ms": 1699999999500,
    "duration_ms": 500
  }
}
```

**Implementation:**
- Already tracking `start_time` in `ActionContext`
- Add `action_completed_time` when action dispatched
- `wait_completed_time` when wait finishes
- Calculate `duration_ms`

**Files to modify:**
- `abp_action_context.h/cc` - Add timing fields
- `abp_controller.cc` - Record timestamps at each stage

---

### 4. Event Collector System

Capture browser events that occur during action execution.

**Component:** `AbpEventCollector` (member of `AbpController`)

**Ownership and Threading:**
- `AbpEventCollector` is owned by `AbpController` as a member variable
- Lives on the UI/browser thread (same as `AbpController`)
- Shares lifetime with `AbpController` - no separate lifecycle management needed
- Single action at a time per tab is acceptable - no concurrent action handling required

**Events to capture:**
| Event | Source | CDP/API |
|-------|--------|---------|
| `navigation` | Page navigated | CDP `Page.frameNavigated` |
| `dialog` | Alert/confirm/prompt | CDP `Page.javascriptDialogOpening` |
| `file_chooser` | File picker opened | CDP `Page.fileChooserOpened` |
| `popup` | New window/tab | Chrome TabStripModel observer |
| `tab_closed` | Tab closed | Chrome TabStripModel observer |
| `scroll` | Page scrolled | CDP `DOM.scrollPositionChanged` or polling |
| `download_started` | Download began | Chrome DownloadManager observer |
| `download_completed` | Download finished | Chrome DownloadManager observer |

**Architecture:**
```
┌─────────────────────────────────────────┐
│              AbpController              │
├─────────────────────────────────────────┤
│ - event_collector_ (member)             │
│ - ... other members ...                 │
└─────────────────────────────────────────┘
         │ owns
         ▼
┌─────────────────────────────────────────┐
│            AbpEventCollector            │
├─────────────────────────────────────────┤
│ - StartCapturing(tab_id)                │
│ - StopCapturing() -> vector<Event>      │
│ - OnCdpEvent(method, params)            │
│ - OnTabCreated(tab_id)                  │
│ - OnTabClosed(tab_id)                   │
│ - OnDownloadStarted(download_id)        │
│ - OnDownloadCompleted(download_id)      │
└─────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────┐
│          Event Buffer (per action)      │
│  vector<AbpEvent> captured_events_      │
└─────────────────────────────────────────┘
```

**Files to create:**
- `abp_event_collector.h`
- `abp_event_collector.cc`

**Files to modify:**
- `abp_controller.h` - Add `std::unique_ptr<AbpEventCollector> event_collector_` member
- `abp_controller.cc` - Initialize collector in constructor, integrate with action flow

---

### 5. CDP Event Subscription

Subscribe to CDP events for capturing.

**Events to enable:**
```cpp
// Enable required CDP domains
client->SendCommand("Page.enable", {}, ...);
client->SendCommand("DOM.enable", {}, ...);

// Subscribe to events
// Page.frameNavigated
// Page.javascriptDialogOpening
// Page.fileChooserOpened
```

**Modify AbpCdpClient:**
- Already has `SetEventListener()` for events
- Route events to `AbpEventCollector`

---

### 6. File Chooser Event with ID

When file chooser opens, generate unique ID and include in event.

**Event format:**
```json
{
  "type": "file_chooser",
  "virtual_time_ms": 1699999999300,
  "data": {
    "id": "fc_abc123",
    "tab_id": "tab_xyz789",
    "chooser_type": "open",
    "accepts": [...],
    "multiple": false,
    "pending": true
  }
}
```

**Implementation:**
- Generate UUID for each file chooser
- Store pending choosers in map: `id -> FileChooserInfo`
- CDP `Page.fileChooserOpened` provides mode and accepts info

**Files to modify:**
- `abp_event_collector.cc` - Handle `Page.fileChooserOpened`
- Add `pending_file_choosers_` map to controller

---

### 7. Dialog Event Capture

Capture dialog events with relevant info.

**Event format:**
```json
{
  "type": "dialog",
  "virtual_time_ms": 1699999999200,
  "data": {
    "tab_id": "tab_abc123",
    "dialog_type": "confirm",
    "message": "Are you sure?",
    "default_prompt": "",
    "pending": true
  }
}
```

**CDP event:** `Page.javascriptDialogOpening`
- `type`: "alert", "confirm", "prompt", "beforeunload"
- `message`: Dialog message text
- `defaultPrompt`: Default text for prompt dialogs

---

### 8. Virtual Time in Events

All events use `virtual_time_ms` from execution control.

**Implementation:**
- Get current virtual time when event occurs
- If execution control disabled, use wall clock time
- Store in event struct

```cpp
int64_t GetVirtualTimeMs(const std::string& tab_id) {
  auto it = execution_states_.find(tab_id);
  if (it != execution_states_.end() && it->second.virtual_time_enabled) {
    return it->second.virtual_time_base_ticks_ms;
  }
  return base::Time::Now().InMillisecondsSinceUnixEpoch();
}
```

---

## Data Structures

### AbpEvent
```cpp
struct AbpEvent {
  std::string type;  // "navigation", "dialog", "file_chooser", etc.
  int64_t virtual_time_ms;
  base::Value::Dict data;
};
```

### ActionResponse (extended)
```cpp
struct ActionResponse {
  // Result
  base::Value::Dict result;

  // Screenshot
  std::string screenshot_base64;
  int screenshot_width = 0;
  int screenshot_height = 0;
  int64_t screenshot_virtual_time_ms = 0;
  std::string screenshot_markup;

  // Scroll
  double scroll_horizontal_percent = 0;
  double scroll_vertical_percent = 0;
  int scroll_horizontal_px = 0;
  int scroll_vertical_px = 0;
  int page_width = 0;
  int page_height = 0;
  int viewport_width = 0;
  int viewport_height = 0;

  // Events
  std::vector<AbpEvent> events;

  // Timing
  int64_t action_started_ms = 0;
  int64_t action_completed_ms = 0;
  int64_t wait_completed_ms = 0;
  int64_t duration_ms = 0;
};
```

---

## Integration with Action Flow

Current flow:
```
Action Request
    ↓
Before Screenshot (history)
    ↓
Dispatch Action
    ↓
Wait for Action Complete
    ↓
After Screenshot (history)
    ↓
Record to History DB
    ↓
Send Response (result only)
```

New flow:
```
Action Request
    ↓
Start Event Capture ← NEW
    ↓
Before Screenshot (history)
    ↓
Dispatch Action
    ↓
Wait for Action Complete
    ↓
Stop Event Capture ← NEW
    ↓
Get Scroll Position ← NEW
    ↓
After Screenshot (history + base64) ← MODIFIED
    ↓
Build Full Response Envelope ← NEW
    ↓
Record to History DB
    ↓
Send Response (full envelope)
```

---

## Testing

### Manual Testing

```bash
# Click and check response envelope
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/click \
  -H "Content-Type: application/json" \
  -d '{"x":100,"y":200}' | jq '.data.screenshot.width, .data.scroll, .data.events'

# Navigate and check for navigation event
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/navigate \
  -d '{"url":"https://example.com"}' | jq '.data.events'

# Trigger alert and check for dialog event
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/execute \
  -d '{"script":"alert(\"test\")"}' | jq '.data.events[] | select(.type==\"dialog\")'
```

---

## Estimated Scope

| Task | Complexity | Lines of Code |
|------|------------|---------------|
| Screenshot in response | Medium | ~100 |
| Scroll position | Low | ~60 |
| Timing in response | Low | ~40 |
| AbpEventCollector class | High | ~300 |
| CDP event subscription | Medium | ~80 |
| File chooser events | Medium | ~100 |
| Dialog events | Low | ~50 |
| Virtual time integration | Low | ~30 |
| Response envelope builder | Medium | ~120 |
| **Total** | | **~880** |

---

## Success Criteria

After Phase 2:
- [ ] All action responses include base64 screenshot
- [ ] All action responses include scroll position
- [ ] All action responses include timing metrics
- [ ] Navigation events captured and returned
- [ ] Dialog events captured with message/type
- [ ] File chooser events include unique ID
- [ ] Popup/tab events captured
- [ ] Download events captured
- [ ] All event timestamps use virtual time
- [ ] Response envelope matches API.md specification
