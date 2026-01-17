# Action & Event History with Database Integration

This document specifies the history tracking and retrieval system for ABP, enabling agents to query past actions, review browser events, and manage session history through persistent storage.

## Overview

The history system records all actions performed through ABP and events captured from the browser, storing them in a SQLite database for efficient querying and persistence across browser restarts.

### Goals

1. **Action History**: Record every API action with parameters, results, and timing
2. **Event History**: Capture browser events as defined in API.md (navigation, dialog, popup, etc.)
3. **Session Management**: Group actions/events by session with lifecycle tracking
4. **Query Flexibility**: Filter by time range, action type, tab, or session
5. **Data Management**: Clear history (selective or complete) to manage storage

### Non-Goals

- Real-time event streaming (future WebSocket feature)
- Cross-browser history sync
- Compression or archival strategies

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    REST API Layer                        │
│   GET/DELETE /api/v1/history/*                          │
└─────────────────────┬───────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────┐
│              AbpHistoryController (UI thread)            │
│   - Query parsing and validation                         │
│   - Coordinates with AbpController for action recording  │
└─────────────────────┬───────────────────────────────────┘
                      │ PostTask
┌─────────────────────▼───────────────────────────────────┐
│              AbpHistoryDatabase (DB thread)              │
│   - SQLite operations                                    │
│   - Schema management                                    │
│   - CRUD operations                                      │
└─────────────────────┬───────────────────────────────────┘
                      │
     ┌────────────────┴────────────────┐
     │                                 │
┌────▼─────────────────┐    ┌──────────▼──────────────┐
│   SQLite Database    │    │   Screenshots Folder    │
│   abp_history.db     │    │   abp_screenshots/      │
└──────────────────────┘    └─────────────────────────┘
```

**Storage locations** (configurable via `abp_config.json`):
- Database: `~/.config/chromium/abp_history.db`
- Screenshots: `~/.config/chromium/abp_screenshots/`
- Config: `~/.config/chromium/abp_config.json`

### Threading Model

| Component | Thread | Responsibility |
|-----------|--------|----------------|
| AbpHttpServer | IO | HTTP request/response |
| AbpHistoryController | UI | Request validation, routing |
| AbpHistoryDatabase | DB (SequencedTaskRunner) | All SQLite operations |

**Critical**: SQLite operations MUST run on a dedicated sequenced task runner to avoid blocking UI thread and ensure thread-safe database access.

### Write Path Architecture

**PostTask-based writes.** Database operations use a dedicated `SequencedTaskRunner` owned by `AbpHistoryDatabase`, following Chrome's standard patterns:

```
Action flow (UI thread):
    │
    ├─► Capture "before" screenshot via CDP
    │       └─► ThreadPool: decode base64, write file
    │
    ├─► Execute action, wait for completion
    │
    ├─► Capture "after" screenshot via CDP
    │       └─► ThreadPool: decode base64, write file
    │
    └─► PostTask to DB SequencedTaskRunner
            └─► INSERT action row with screenshot paths

Event flow (UI thread):
    │
    └─► PostTask to DB SequencedTaskRunner
            └─► INSERT event row
```

**Design:**
- `AbpHistoryDatabase` owns the `base::SequencedTaskRunner` for all SQLite operations
- Screenshot file writes use `base::ThreadPool` (same pattern as CDP screenshot save)
- UI thread posts tasks via `PostTask` with `base::BindOnce`
- `weak_factory_` pattern to prevent use-after-free on shutdown

**Flush on shutdown**: Use `base::RunLoop` with the sequenced task runner to drain pending writes during browser shutdown.

### Integration with AbpController

```cpp
// In AbpController - wrap every action handler
void AbpController::Navigate(const std::string& tab_id,
                             const base::Value::Dict& params,
                             ResponseCallback callback) {
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();

  // Perform action (existing code)
  content::WebContents* wc = FindWebContents(tab_id);
  // ...

  // Record action asynchronously (non-blocking)
  int64_t end_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  history_controller_->RecordAction(
      session_id_, tab_id, "navigate", params, result, success,
      start_time, end_time - start_time);

  // Return response (existing code)
  SendJson(200, std::move(result), std::move(callback));
}
```

## Database Schema

### Timestamp-Based Correlation

Actions and events are independent records linked by timestamps, not foreign keys. This design:
- Allows events to exist without a preceding action (user manual interaction, timers, etc.)
- Simplifies the write path (no need to track "current action")
- Enables flexible querying via timestamp ranges

```
Timeline (all records have timestamps):
  t=1000  Action: navigate to example.com
  t=1050  Event: navigation started
  t=1100  Event: network request
  t=1500  Event: navigation complete
  t=2000  Action: click at (100, 200)
  t=2050  Event: click dispatched
  t=3000  Event: console log (no preceding ABP action - user interaction)
```

**Querying events for an action:**
```sql
-- Find events that occurred during/after action, before next action
SELECT e.* FROM events e
WHERE e.session_id = ?
  AND e.timestamp >= ?  -- action start time
  AND e.timestamp < COALESCE(
    (SELECT MIN(a2.timestamp) FROM actions a2
     WHERE a2.session_id = e.session_id AND a2.timestamp > ?),
    9223372036854775807  -- MAX_INT64, includes all if no next action
  )
ORDER BY e.timestamp;
```

**Benefits:**
- Events captured during idle periods are preserved (not discarded)
- No circular dependency between action recording and event capture
- Simpler write path - just INSERT with current timestamp

### Tables

```sql
-- Session tracking
CREATE TABLE sessions (
  id TEXT PRIMARY KEY,           -- UUID
  start_time INTEGER NOT NULL,   -- Unix timestamp ms
  end_time INTEGER,              -- NULL if active
  browser_version TEXT,
  user_agent TEXT
);

-- Action history
CREATE TABLE actions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id TEXT NOT NULL REFERENCES sessions(id),
  tab_id TEXT,                   -- NULL for non-tab actions
  action_type TEXT NOT NULL,     -- "navigate", "click", "type", etc.
  timestamp INTEGER NOT NULL,    -- Unix timestamp ms
  duration_ms INTEGER,           -- Time to complete
  params TEXT,                   -- JSON parameters
  result TEXT,                   -- JSON result
  success INTEGER NOT NULL,      -- 1 = success, 0 = error
  error_code TEXT,               -- Error code if failed
  error_message TEXT,            -- Error message if failed
  screenshot_before_path TEXT,   -- Path to WebP before action, NULL if disabled
  screenshot_after_path TEXT     -- Path to WebP after action wait, NULL if disabled
);

-- Browser events
CREATE TABLE events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id TEXT NOT NULL REFERENCES sessions(id),
  tab_id TEXT,
  event_type TEXT NOT NULL,      -- "navigation", "dialog", "popup", etc.
  timestamp INTEGER NOT NULL,    -- Unix timestamp ms, used for action correlation
  data TEXT                      -- JSON event data
);

-- Indexes for common queries
CREATE INDEX idx_actions_session ON actions(session_id);
CREATE INDEX idx_actions_tab ON actions(tab_id);
CREATE INDEX idx_actions_type ON actions(action_type);
CREATE INDEX idx_actions_timestamp ON actions(timestamp);
-- Composite index for finding next action in session (for event correlation)
CREATE INDEX idx_actions_session_timestamp ON actions(session_id, timestamp);
-- Composite index for cursor-based pagination within a session
CREATE INDEX idx_actions_session_id ON actions(session_id, id);
CREATE INDEX idx_events_session ON events(session_id);
CREATE INDEX idx_events_tab ON events(tab_id);
CREATE INDEX idx_events_type ON events(event_type);
CREATE INDEX idx_events_timestamp ON events(timestamp);
-- Composite index for efficient action-event correlation queries
CREATE INDEX idx_events_session_timestamp ON events(session_id, timestamp);
-- Composite index for cursor-based pagination within a session
CREATE INDEX idx_events_session_id ON events(session_id, id);
```


## REST API Specification

### Pagination

All list endpoints use **cursor-based pagination** for O(1) performance regardless of dataset size. This avoids the O(n) offset scanning problem with traditional limit/offset pagination.

**Cursor format:**
- For actions and events: The cursor is the `id` of the last item returned (integer)
- For sessions: The cursor is the `start_time` of the last session returned (integer timestamp)

**Direction relative to time:**
- `"forward"` (default): Move toward **newer** items (higher IDs/timestamps)
- `"backward"`: Move toward **older** items (lower IDs/timestamps)

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `cursor` | string | Cursor from previous response. Omit for first page (returns newest items). |
| `limit` | integer | Max items to return (endpoint-specific defaults and maximums) |
| `direction` | string | `"forward"` (toward newer) or `"backward"` (toward older, default for initial query) |

**Response pagination fields:**
```json
{
  "data": {
    "items": [...],
    "next_cursor": "1234",      // cursor for next page (newer items), null if none
    "prev_cursor": "1200",      // cursor for previous page (older items), null if none
    "has_more": true            // true if more items exist in current direction
  }
}
```

**SQL implementation:**
```sql
-- Initial query (no cursor): get newest items first
SELECT * FROM actions
WHERE session_id = ?
ORDER BY id DESC
LIMIT ?;

-- Forward (toward newer): get items with id > cursor
SELECT * FROM actions
WHERE session_id = ? AND id > ?
ORDER BY id ASC
LIMIT ?;

-- Backward (toward older): get items with id < cursor
SELECT * FROM actions
WHERE session_id = ? AND id < ?
ORDER BY id DESC
LIMIT ?;
```

**Usage example:**
```bash
# First page (newest items)
curl "http://localhost:8222/api/v1/history/actions?limit=50"
# Response: items [id=1250..1201], prev_cursor="1201", next_cursor=null (at newest)

# Get older items
curl "http://localhost:8222/api/v1/history/actions?limit=50&cursor=1201&direction=backward"
# Response: items [id=1200..1151], prev_cursor="1151", next_cursor="1200"

# Go back to newer items
curl "http://localhost:8222/api/v1/history/actions?limit=50&cursor=1200&direction=forward"
```

### Session Endpoints

#### List Sessions

```
GET /api/v1/history/sessions
```

**Query Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| limit | integer | No | Max sessions to return (default: 20, max: 100) |
| cursor | string | No | Cursor from previous response (start_time of last session) |
| direction | string | No | `"forward"` (default, most recent first) or `"backward"` |
| active | boolean | No | Filter to active sessions only |

**Response:**
```json
{
  "success": true,
  "data": {
    "sessions": [
      {
        "id": "550e8400-e29b-41d4-a716-446655440000",
        "start_time": 1705500000000,
        "end_time": null,
        "browser_version": "122.0.6261.0"
      }
    ],
    "prev_cursor": "1705400000000",
    "next_cursor": null,
    "has_more": true
  }
}
```

#### Get Current Session

```
GET /api/v1/history/sessions/current
```

**Response:**
```json
{
  "success": true,
  "data": {
    "id": "550e8400-e29b-41d4-a716-446655440000",
    "start_time": 1705500000000,
    "end_time": null,
    "browser_version": "122.0.6261.0",
    "user_agent": "Mozilla/5.0 ..."
  }
}
```

#### Get Session Details

```
GET /api/v1/history/sessions/{session_id}
```

**Response:**
```json
{
  "success": true,
  "data": {
    "id": "550e8400-e29b-41d4-a716-446655440000",
    "start_time": 1705500000000,
    "end_time": 1705503600000,
    "browser_version": "122.0.6261.0",
    "user_agent": "Mozilla/5.0 ..."
  }
}
```

### Action History Endpoints

#### List Actions

```
GET /api/v1/history/actions
```

**Query Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| session_id | string | No | Filter by session (default: current) |
| tab_id | string | No | Filter by tab |
| action_type | string | No | Filter by type ("navigate", "click", etc.) |
| success | boolean | No | Filter by success/failure |
| start_time | integer | No | Unix timestamp ms, inclusive |
| end_time | integer | No | Unix timestamp ms, inclusive |
| limit | integer | No | Max actions (default: 50, max: 500) |
| cursor | string | No | Cursor from previous response (action id) |
| direction | string | No | `"forward"` (default, newer first) or `"backward"` |

**Response:**
```json
{
  "success": true,
  "data": {
    "actions": [
      {
        "id": 1234,
        "session_id": "550e8400-e29b-41d4-a716-446655440000",
        "tab_id": "ABCD1234",
        "action_type": "navigate",
        "timestamp": 1705500100000,
        "duration_ms": 1523,
        "success": true,
        "params": {
          "url": "https://example.com"
        },
        "result": {
          "url": "https://example.com/",
          "title": "Example Domain"
        },
        "screenshot_before_path": "/home/user/.config/chromium/abp_screenshots/1705500100000_ABCD1234_before.webp",
        "screenshot_after_path": "/home/user/.config/chromium/abp_screenshots/1705500101523_ABCD1234_after.webp"
      }
    ],
    "prev_cursor": "1234",
    "next_cursor": null,
    "has_more": true
  }
}
```

#### Get Action Details

```
GET /api/v1/history/actions/{action_id}
```

**Response:**
```json
{
  "success": true,
  "data": {
    "id": 1234,
    "session_id": "550e8400-e29b-41d4-a716-446655440000",
    "tab_id": "ABCD1234",
    "action_type": "navigate",
    "timestamp": 1705500100000,
    "duration_ms": 1523,
    "success": true,
    "params": {
      "url": "https://example.com"
    },
    "result": {
      "url": "https://example.com/",
      "title": "Example Domain"
    },
    "screenshot_before_path": "/home/user/.config/chromium/abp_screenshots/1705500100000_ABCD1234_before.webp",
    "screenshot_after_path": "/home/user/.config/chromium/abp_screenshots/1705500101523_ABCD1234_after.webp"
  }
}
```

#### Get Action Screenshot

```
GET /api/v1/history/actions/{action_id}/screenshot?type=before
GET /api/v1/history/actions/{action_id}/screenshot?type=after
```

**Query Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| type | string | No | `"before"` or `"after"` (default: `"after"`) |

**Response:**
Binary WebP image data with `Content-Type: image/webp` header.

**Note:** Reads the file from `screenshot_before_path` or `screenshot_after_path`. Returns 404 if screenshots were disabled or file is missing.

#### Clear Actions

```
DELETE /api/v1/history/actions
```

**Query Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| session_id | string | No | Clear only this session's actions |
| tab_id | string | No | Clear only this tab's actions |
| before | integer | No | Clear actions before this timestamp |

**Response:**
```json
{
  "success": true,
  "data": {
    "deleted_count": 42
  }
}
```

### Event History Endpoints

#### List Events

```
GET /api/v1/history/events
```

**Query Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| session_id | string | No | Filter by session (default: current) |
| tab_id | string | No | Filter by tab |
| event_type | string | No | Filter by type ("navigation", "dialog", "popup", etc.) |
| start_time | integer | No | Unix timestamp ms, inclusive |
| end_time | integer | No | Unix timestamp ms, inclusive |
| limit | integer | No | Max events (default: 100, max: 1000) |
| cursor | string | No | Cursor from previous response (event id) |
| direction | string | No | `"forward"` (default, newer first) or `"backward"` |

**Response:**
```json
{
  "success": true,
  "data": {
    "events": [
      {
        "id": 5678,
        "session_id": "550e8400-e29b-41d4-a716-446655440000",
        "tab_id": "ABCD1234",
        "event_type": "navigation",
        "timestamp": 1705500100500,
        "data": {
          "url": "https://example.com/",
          "transitionType": "typed"
        }
      }
    ],
    "prev_cursor": "5678",
    "next_cursor": null,
    "has_more": true
  }
}
```

#### Get Event Details

```
GET /api/v1/history/events/{event_id}
```

**Response:**
```json
{
  "success": true,
  "data": {
    "id": 5678,
    "session_id": "550e8400-e29b-41d4-a716-446655440000",
    "tab_id": "ABCD1234",
    "event_type": "navigation",
    "timestamp": 1705500100500,
    "data": {
      "url": "https://example.com/",
      "transitionType": "typed"
    }
  }
}
```

#### Clear Events

```
DELETE /api/v1/history/events
```

**Query Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| session_id | string | No | Clear only this session's events |
| tab_id | string | No | Clear only this tab's events |
| event_type | string | No | Clear only this event type |
| before | integer | No | Clear events before this timestamp |

**Response:**
```json
{
  "success": true,
  "data": {
    "deleted_count": 156
  }
}
```

### Bulk Operations

#### Clear All History

```
DELETE /api/v1/history
```

**Query Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| session_id | string | No | Clear only this session |
| confirm | boolean | Yes | Must be true to confirm deletion |

**Response:**
```json
{
  "success": true,
  "data": {
    "deleted_sessions": 1,
    "deleted_actions": 42,
    "deleted_events": 156
  }
}
```

#### Export Session

```
GET /api/v1/history/sessions/{session_id}/export
```

**Query Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| include_screenshots | boolean | No | Include base64 screenshots (default: false) |
| actions_cursor | string | No | Resume export from this action cursor |
| events_cursor | string | No | Resume export from this event cursor |
| chunk_size | integer | No | Max items per type (default: 1000, max: 5000) |

**Response:**
Session data as JSON with cursor-based chunking to prevent memory exhaustion on large sessions.

```json
{
  "success": true,
  "data": {
    "session": { /* session metadata */ },
    "actions": [ /* chunk of actions; if include_screenshots=true, adds base64 "screenshot_before" and "screenshot_after" fields */ ],
    "events": [ /* chunk of events */ ],
    "next_actions_cursor": "5000",
    "next_events_cursor": "25000",
    "export_complete": false
  }
}
```

**Usage for large sessions:**
```bash
# First chunk
curl ".../export?chunk_size=1000"
# Response: next_actions_cursor=1000, next_events_cursor=5000, export_complete=false

# Continue
curl ".../export?chunk_size=1000&actions_cursor=1000&events_cursor=5000"
# Response: next_actions_cursor=2000, next_events_cursor=10000, export_complete=false

# ... repeat until export_complete=true
```

## Event Types

Event types match those defined in `plans/API.md`:

| Event Type | Description | CDP Source |
|------------|-------------|------------|
| `navigation` | Page navigated to new URL | Page.frameNavigated |
| `dialog` | Browser dialog appeared (alert, confirm, prompt) | Page.javascriptDialogOpening |
| `file_chooser` | Native file picker dialog appeared | Page.fileChooserOpened |
| `popup` | New popup window or tab opened | Target.targetCreated |
| `tab_closed` | Tab was closed | Target.targetDestroyed |
| `scroll` | Page was scrolled | (DOM event listener) |
| `download_started` | Download was initiated | Browser.downloadWillBegin |
| `download_completed` | Download finished successfully | Browser.downloadProgress |
| `file_selected` | Files were selected in file chooser | Page.fileChooserOpened (response) |
| `file_chooser_cancelled` | File chooser dismissed without selection | Page.fileChooserOpened (cancelled) |

## Implementation Plan

### Phase 0: Configuration

**Files:**
- `chrome/browser/abp/abp_config.h`
- `chrome/browser/abp/abp_config.cc`

**Tasks:**
- [ ] Define config struct with history settings
- [ ] Parse JSON config file from `~/.config/chromium/abp_config.json`
- [ ] Support `--abp-config=/path` override flag
- [ ] Validate config values and apply defaults for missing fields
- [ ] Unit tests for config parsing

### Phase 1: Database Foundation

**Files:**
- `chrome/browser/abp/abp_history_database.h`
- `chrome/browser/abp/abp_history_database.cc`

**Tasks:**
- [ ] Create SQLite database wrapper owning a `base::SequencedTaskRunner`
- [ ] Enable WAL mode for concurrent read performance
- [ ] Implement schema creation (create tables if not exist)
- [ ] Add CRUD operations for sessions, actions, events
- [ ] Add query builders with filtering and cursor-based pagination
- [ ] Implement flush on shutdown via `RunLoop` on the sequenced runner
- [ ] Unit tests for database operations

### Phase 2: History Controller

**Files:**
- `chrome/browser/abp/abp_history_controller.h`
- `chrome/browser/abp/abp_history_controller.cc`

**Tasks:**
- [ ] Create controller with config and database dependencies
- [ ] Implement session lifecycle (create on start, close on exit)
- [ ] Add `RecordAction()`:
  - Capture before screenshot via CDP (ThreadPool)
  - Execute action
  - Capture after screenshot via CDP (ThreadPool)
  - Write screenshot files to disk (ThreadPool, following CDP pattern)
  - PostTask to DB runner for action INSERT with paths
- [ ] Add `RecordEvent()`: PostTask to DB runner for event INSERT
- [ ] Wire up to AbpHttpServer routing

### Phase 3: AbpController Integration

**Files:**
- `chrome/browser/abp/abp_controller.cc` (modify)

**Tasks:**
- [ ] Inject AbpHistoryController dependency
- [ ] Wrap action handlers with recording calls
- [ ] Generate session ID on ABP initialization
- [ ] Pass session context through action pipeline

### Phase 4: Event Capture

**Files:**
- `chrome/browser/abp/abp_event_observer.h`
- `chrome/browser/abp/abp_event_observer.cc`

**Tasks:**
- [ ] Subscribe to CDP events via DevToolsAgentHost
- [ ] Capture events matching API.md types: navigation, dialog, file_chooser, popup, tab_closed, scroll, download_started, download_completed, file_selected, file_chooser_cancelled
- [ ] Transform events to storage format matching API.md schema
- [ ] Record events with high-precision timestamps for later correlation with actions
- [ ] PostTask to DB task runner for INSERT (non-blocking)
- [ ] Rate limit high-frequency events (scroll: max 1 per 100ms)

**Note:** Events are independent of actions. Correlation is done at query time using timestamp ranges, not at write time.

### Phase 5: REST Endpoints

**Files:**
- `chrome/browser/abp/abp_http_server.cc` (modify)
- `chrome/browser/abp/abp_history_controller.cc` (add handlers)

**Tasks:**
- [ ] Add `/history/*` route parsing
- [ ] Implement cursor-based pagination for all list endpoints
- [ ] Parse cursor/direction params, validate cursor format
- [ ] Generate prev_cursor/next_cursor/has_more in responses
- [ ] Implement all GET endpoints (list, detail)
- [ ] Implement DELETE endpoints (clear, also deletes screenshot files)
- [ ] Add chunked export functionality with cursor resume
- [ ] Add screenshot endpoint with `?type=before|after` parameter

### Phase 6: MCP Server Integration

**Files:**
- `tools/abp-mcp-server/src/index.ts` (modify)

**Tasks:**
- [ ] Add history tool definitions
- [ ] Implement history query tools
- [ ] Add history clear tools
- [ ] Update documentation

## Configuration

History settings are configured via a JSON config file rather than command-line flags, keeping ABP configuration centralized and easier to manage.

### Config File Location

```
~/.config/chromium/abp_config.json
```

Override with: `--abp-config=/path/to/config.json`

**If no config file exists:** ABP uses built-in defaults (history enabled, screenshots enabled). The config file is optional.

### Config File Schema

```json
{
  "history": {
    "enabled": true,
    "database_path": "~/.config/chromium/abp_history.db",
    "screenshots": {
      "enabled": true,
      "directory": "~/.config/chromium/abp_screenshots/"
    }
  }
}
```

### Config Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `history.enabled` | boolean | `true` | Enable/disable history recording |
| `history.database_path` | string | `~/.config/chromium/abp_history.db` | SQLite database location |
| `history.screenshots.enabled` | boolean | `true` | Save before/after screenshots for each action |
| `history.screenshots.directory` | string | `~/.config/chromium/abp_screenshots/` | Folder for WebP files |

### Screenshot Storage

Screenshots are saved as WebP files in a single flat folder:

```
~/.config/chromium/abp_screenshots/
├── 1705500100000_ABCD1234_before.webp
├── 1705500101523_ABCD1234_after.webp
├── 1705500200000_ABCD1234_before.webp
├── 1705500201200_ABCD1234_after.webp
└── ...
```

**File naming:** `{timestamp_ms}_{tab_id}_{before|after}.webp`

- **Before screenshot**: Captured immediately before action dispatch
- **After screenshot**: Captured after action wait condition completes

Paths are stored in `actions.screenshot_before_path` and `actions.screenshot_after_path`.

## Error Codes

| Code | Description |
|------|-------------|
| `HISTORY_DISABLED` | History recording is disabled in config |
| `SESSION_NOT_FOUND` | Specified session does not exist |
| `ACTION_NOT_FOUND` | Specified action does not exist |
| `EVENT_NOT_FOUND` | Specified event does not exist |
| `SCREENSHOT_NOT_FOUND` | Screenshot file missing or path is NULL |
| `DATABASE_ERROR` | SQLite operation failed |
| `STORAGE_ERROR` | Screenshot file write/delete failed |
| `CONFIG_ERROR` | Config file parse error or invalid values |
| `INVALID_QUERY` | Invalid query parameters |
| `INVALID_CURSOR` | Cursor value is malformed or references deleted record |
| `EXPORT_FAILED` | Export operation failed |

## Security Considerations

1. **Local Access Only**: History endpoints only accessible via localhost
2. **No Sensitive Data**: Avoid storing passwords, tokens, or PII in action params
3. **Database Permissions**: SQLite file readable only by current user
4. **Clear on Request**: Users can clear all history at any time
5. **No Remote Sync**: History never leaves the local machine

## Future Enhancements

- **WebSocket Streaming**: Real-time event stream endpoint
- **Query Language**: SQL-like filtering syntax
- **Replay**: Re-execute recorded actions
- **Diff View**: Compare two sessions
- **Analytics**: Action timing statistics, success rates
