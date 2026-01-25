# Phase 3: Dialogs, Downloads, File Chooser & Browser Control

## Goal

Complete the remaining API endpoints for handling browser dialogs, managing downloads, responding to file choosers, and browser lifecycle control. After this phase, the ABP implementation matches the full API specification.

## Endpoints to Implement

### 1. Dialog Endpoints

Handle JavaScript dialogs (alert, confirm, prompt, beforeunload).

#### GET /tabs/{tab_id}/dialog

Check for pending dialog.

**Response:**
```json
{
  "success": true,
  "data": {
    "present": true,
    "dialog_type": "confirm",
    "message": "Are you sure you want to delete?",
    "default_prompt": ""
  }
}
```

**Implementation:**
- Track pending dialogs from `Page.javascriptDialogOpening` events
- Store in `pending_dialogs_` map keyed by tab_id
- Return current pending dialog info or `present: false`

#### POST /tabs/{tab_id}/dialog/accept

Accept the pending dialog.

**Request:**
```json
{
  "prompt_text": "User input for prompt dialogs"
}
```

**Implementation:**
- Use CDP `Page.handleJavaScriptDialog`
- Set `accept: true`
- Include `promptText` if provided

**CDP Command:**
```json
{
  "method": "Page.handleJavaScriptDialog",
  "params": {
    "accept": true,
    "promptText": "user input"
  }
}
```

#### POST /tabs/{tab_id}/dialog/dismiss

Dismiss the pending dialog.

**Implementation:**
- Use CDP `Page.handleJavaScriptDialog` with `accept: false`

**Files to create/modify:**
- `abp_controller.cc` - Add `GetDialog()`, `AcceptDialog()`, `DismissDialog()` methods
- Add `pending_dialogs_` map to track open dialogs
- Subscribe to `Page.javascriptDialogOpening` and `Page.javascriptDialogClosed`

---

### 2. Download Endpoints

Monitor and manage downloads. Configuration is via ABP config at launch.

#### GET /downloads

List downloads.

**Query params:**
- `state=in_progress` - Filter by state
- `limit=100` - Max entries

**Response:**
```json
{
  "success": true,
  "data": {
    "downloads": [
      {
        "id": "dl_123",
        "url": "https://example.com/file.pdf",
        "filename": "file.pdf",
        "path": "/downloads/file.pdf",
        "state": "completed",
        "bytes_received": 102400,
        "total_bytes": 102400,
        "mime_type": "application/pdf",
        "start_time": 1699999999000,
        "end_time": 1699999999500
      }
    ]
  }
}
```

**Implementation:**
- Create `AbpDownloadObserver` to track downloads
- Observe `DownloadManager` for new downloads
- Store download info in map

#### GET /downloads/{download_id}

Get status of specific download.

**Response:**
```json
{
  "success": true,
  "data": {
    "id": "dl_123",
    "state": "in_progress",
    "bytes_received": 51200,
    "total_bytes": 102400,
    "percent_complete": 50
  }
}
```

#### POST /downloads/{download_id}/cancel

Cancel an in-progress download.

**Implementation:**
- Find download by ID
- Call `download::DownloadItem::Cancel(true)`

**Files to create:**
- `abp_download_observer.h`
- `abp_download_observer.cc`

**Files to modify:**
- `abp_controller.cc` - Add download endpoints
- `abp_http_server.cc` - Wire up download observer

**Chrome APIs:**
```cpp
// Get DownloadManager
content::DownloadManager* manager =
    browser->profile()->GetDownloadManager();

// Observe downloads
class AbpDownloadObserver : public download::DownloadItem::Observer {
  void OnDownloadUpdated(download::DownloadItem* item) override;
  void OnDownloadDestroyed(download::DownloadItem* item) override;
};
```

---

### 3. File Chooser Endpoint

Respond to pending file chooser dialogs.

#### POST /file-chooser/{chooser_id}

Provide files to a pending file chooser.

**Request (open dialog):**
```json
{
  "files": ["/path/to/file1.pdf", "/path/to/file2.png"]
}
```

**Request (save dialog):**
```json
{
  "path": "/path/to/save/output.pdf"
}
```

**Request (cancel):**
```json
{
  "cancel": true
}
```

**Implementation:**
- Track pending file choosers from `Page.fileChooserOpened` events
- Store with generated UUID in `pending_file_choosers_` map
- On POST, use CDP `Page.handleFileChooser` to provide files

**CDP Command:**
```json
{
  "method": "Page.handleFileChooser",
  "params": {
    "action": "accept",
    "files": ["/path/to/file.pdf"]
  }
}
```

For cancel:
```json
{
  "method": "Page.handleFileChooser",
  "params": {
    "action": "cancel"
  }
}
```

**Data structures:**
```cpp
struct PendingFileChooser {
  std::string id;           // UUID
  std::string tab_id;
  std::string chooser_type; // "open", "open_multiple", "save"
  bool multiple;
  std::vector<std::string> accepted_extensions;
  int64_t opened_at_ms;
};

std::map<std::string, PendingFileChooser> pending_file_choosers_;
```

**Files to modify:**
- `abp_controller.cc` - Add `HandleFileChooser()` method
- Handle `Page.fileChooserOpened` events to track pending choosers
- Enable `Page.setInterceptFileChooserDialog(enabled: true)`

---

### 4. Browser Shutdown

Graceful browser shutdown.

#### POST /browser/shutdown

**Request:**
```json
{
  "timeout_ms": 5000
}
```

**Implementation:**
- Close all tabs gracefully
- Wait for pending downloads
- Call `chrome::CloseAllBrowsers()`

**Files to modify:**
- `abp_controller.cc` - Add `ShutdownBrowser()` method

**Chrome API:**
```cpp
#include "chrome/browser/lifetime/application_lifetime.h"

void AbpController::ShutdownBrowser(int timeout_ms, ResponseCallback callback) {
  // Option 1: Close all browsers
  chrome::CloseAllBrowsers();

  // Option 2: Exit application
  chrome::AttemptExit();
}
```

---

### 5. Binary Screenshot Endpoint

Return screenshot as binary WebP (not base64 JSON).

#### Prerequisite: Update ResponseCallback Signature

Before implementing binary screenshot, the `ResponseCallback` type must be updated to support binary responses with custom content types.

**Current signature:**
```cpp
using ResponseCallback = base::OnceCallback<void(int status, std::string body)>;
```

**New signature:**
```cpp
using ResponseCallback = base::OnceCallback<void(int status, std::string content_type, std::string body)>;
```

**Files to modify for this change:**
- `abp_http_server.h` - Update `ResponseCallback` typedef
- `abp_http_server.cc` - Update `SendResponse()` to use content_type parameter instead of hardcoding `application/json`
- `abp_controller.h` - Update `ResponseCallback` typedef (if duplicated)
- `abp_controller.cc` - Update all callback invocations to include content_type:
  - JSON responses: `std::move(callback).Run(200, "application/json", json_body);`
  - Binary responses: `std::move(callback).Run(200, "image/webp", binary_data);`

This change must be completed before the binary screenshot endpoint can be implemented.

#### GET /tabs/{tab_id}/screenshot

**Query params:**
- `markup=none` - Markup mode
- `full_page=false` - Capture full page

**Response:**
- `Content-Type: image/webp`
- Raw binary WebP data

**Implementation:**
- Capture screenshot via existing method
- Return raw bytes instead of base64 JSON
- Use new `ResponseCallback` signature to set `image/webp` content type

**Files to modify:**
- `abp_controller.cc` - Add binary screenshot handler, use `"image/webp"` content type
- `abp_http_server.cc` - Already updated in prerequisite step

---

## Configuration Integration

### ABP Config File

Ensure config file controls:
- Window size at launch
- Download directory
- Download auto-accept behavior
- Default file chooser files

**Config structure:**
```json
{
  "window": {
    "width": 1280,
    "height": 720,
    "x": 0,
    "y": 0
  },
  "downloads": {
    "path": "/tmp/abp-downloads",
    "auto_accept": true
  },
  "file_chooser": {
    "auto_accept": false,
    "default_files": []
  }
}
```

**Files to modify:**
- `abp_config.cc` - Parse download and file chooser config
- Apply config on browser launch

---

## Data Structures

### PendingDialog
```cpp
struct PendingDialog {
  std::string tab_id;
  std::string dialog_type;  // "alert", "confirm", "prompt", "beforeunload"
  std::string message;
  std::string default_prompt;
  int64_t opened_at_ms;
};

std::map<std::string, PendingDialog> pending_dialogs_;
```

### TrackedDownload
```cpp
struct TrackedDownload {
  std::string id;
  std::string url;
  std::string filename;
  std::string path;
  std::string state;  // "in_progress", "completed", "cancelled", "failed"
  int64_t bytes_received;
  int64_t total_bytes;
  std::string mime_type;
  int64_t start_time_ms;
  int64_t end_time_ms;
};

std::map<std::string, TrackedDownload> tracked_downloads_;
```

---

## CDP Domain Enablement

Ensure all required CDP domains are enabled:

```cpp
void AbpController::EnableCdpDomains(AbpCdpClient* client) {
  // Already enabled
  client->SendCommand("Page.enable", {}, ...);

  // New for Phase 3
  client->SendCommand("Page.setInterceptFileChooserDialog",
                      {{"enabled", true}}, ...);
}
```

---

## Testing

### Dialog Testing

```bash
# Trigger alert
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/execute \
  -d '{"script":"alert(\"Hello\")"}'

# Check for dialog
curl http://localhost:8222/api/v1/tabs/TAB_ID/dialog

# Accept dialog
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/dialog/accept

# Trigger confirm
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/execute \
  -d '{"script":"confirm(\"Delete?\")"}'

# Dismiss dialog
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/dialog/dismiss

# Trigger prompt
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/execute \
  -d '{"script":"prompt(\"Name?\", \"default\")"}'

# Accept with text
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/dialog/accept \
  -d '{"prompt_text":"John"}'
```

### Download Testing

```bash
# Navigate to trigger download
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/navigate \
  -d '{"url":"https://example.com/file.pdf"}'

# List downloads
curl http://localhost:8222/api/v1/downloads

# Get specific download
curl http://localhost:8222/api/v1/downloads/dl_123

# Cancel download
curl -X POST http://localhost:8222/api/v1/downloads/dl_123/cancel
```

### File Chooser Testing

```bash
# Click file input to trigger chooser
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/click \
  -d '{"x":100,"y":200}'

# Check events for file_chooser with ID
# Response includes: {"events":[{"type":"file_chooser","data":{"id":"fc_abc123",...}}]}

# Provide files
curl -X POST http://localhost:8222/api/v1/file-chooser/fc_abc123 \
  -d '{"files":["/path/to/file.pdf"]}'

# Or cancel
curl -X POST http://localhost:8222/api/v1/file-chooser/fc_abc123 \
  -d '{"cancel":true}'
```

### Binary Screenshot Testing

```bash
# Get binary screenshot
curl http://localhost:8222/api/v1/tabs/TAB_ID/screenshot -o screenshot.webp

# With markup
curl "http://localhost:8222/api/v1/tabs/TAB_ID/screenshot?markup=interactive" \
  -o screenshot_marked.webp

# Check file type
file screenshot.webp
# Output: screenshot.webp: RIFF (little-endian) data, Web/P image
```

### Browser Shutdown Testing

```bash
# Shutdown browser
curl -X POST http://localhost:8222/api/v1/browser/shutdown \
  -d '{"timeout_ms":5000}'
```

---

## Estimated Scope

| Task | Complexity | Lines of Code |
|------|------------|---------------|
| Dialog get endpoint | Low | ~40 |
| Dialog accept endpoint | Low | ~50 |
| Dialog dismiss endpoint | Low | ~30 |
| Dialog event tracking | Medium | ~80 |
| Download observer | High | ~200 |
| Download list endpoint | Low | ~50 |
| Download status endpoint | Low | ~40 |
| Download cancel endpoint | Low | ~30 |
| File chooser tracking | Medium | ~100 |
| File chooser endpoint | Medium | ~80 |
| Browser shutdown | Low | ~50 |
| Binary screenshot | Low | ~60 |
| Config integration | Medium | ~100 |
| **Total** | | **~910** |

---

## Success Criteria

After Phase 3:
- [ ] Can detect pending dialogs via GET
- [ ] Can accept dialogs (with prompt text)
- [ ] Can dismiss dialogs
- [ ] Can list all downloads with filtering
- [ ] Can get individual download status
- [ ] Can cancel in-progress downloads
- [ ] Can provide files to file chooser by ID
- [ ] Can cancel file chooser by ID
- [ ] Can get binary screenshot via GET
- [ ] Can gracefully shutdown browser
- [ ] Download path controlled by config
- [ ] File chooser auto-accept controlled by config

---

## Final Implementation Status

After all three phases:

| Category | Endpoints | Status |
|----------|-----------|--------|
| Tab Management | 5 | ✅ 100% |
| Navigation | 5 | ✅ 100% |
| Mouse Actions | 3 | ✅ 100% |
| Keyboard Actions | 4 | ✅ 100% |
| JavaScript | 1 | ✅ 100% |
| Screenshots | 2 | ✅ 100% |
| Dialogs | 3 | ✅ 100% |
| Downloads | 3 | ✅ 100% |
| File Chooser | 1 | ✅ 100% |
| Execution Control | 2 | ✅ 100% |
| Browser | 2 | ✅ 100% |
| **Total** | **31** | **✅ 100%** |

| Response Envelope | Status |
|-------------------|--------|
| Screenshot in response | ✅ |
| Scroll position | ✅ |
| Events array | ✅ |
| Virtual time timestamps | ✅ |
| Timing metrics | ✅ |
