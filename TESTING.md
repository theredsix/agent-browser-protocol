# ABP Testing Guide

This document describes the testing infrastructure for the Agent Browser Protocol.

## Quick Start

```bash
# 1. Start Chrome with ABP enabled
./out/Default/chrome --enable-abp

# 2. Start HTTP server for test pages (separate terminal)
cd chrome/browser/abp/test_pages
python3 -m http.server 8081

# 3. Run the integration test suite
./run_tests.sh
```

## Test Suites

### REST API Integration Tests

**Location:** `chrome/browser/abp/test_pages/run_tests.sh`

| Test ID | Description | Endpoints Tested |
|---------|-------------|------------------|
| NAV-001 | Navigate to URL | `POST /tabs/{id}/navigate` |
| NAV-003 | Back/Forward navigation | `POST /tabs/{id}/back`, `/forward` |
| CLICK-001 | Click button | `POST /tabs/{id}/click` |
| TYPE-001 | Type text into input | `POST /tabs/{id}/type` |
| KEY-001 | Press Enter for form submit | `POST /tabs/{id}/keyboard/press` |
| EXEC-001 | Execute JavaScript | `POST /tabs/{id}/execute` |
| SCREENSHOT-001 | Take screenshot | `POST /tabs/{id}/screenshot` |
| VTIME-001 | Timer frozen when paused | `GET/POST /tabs/{id}/execution` |
| VTIME-002 | Date.now frozen when paused | Virtual time verification |
| VTIME-003 | setTimeout not firing when paused | Virtual time verification |

**Requirements:**
- ABP server on `http://localhost:8222`
- Test page HTTP server on `http://localhost:8081`
- `jq` command-line tool installed

### MCP Server Tests

**Location:** `tools/abp-mcp-test.sh`

Tests the embedded MCP server (JSON-RPC over HTTP):

| Test | Description |
|------|-------------|
| 1 | MCP Initialize (protocol handshake) |
| 2 | notifications/initialized |
| 3 | tools/list (enumerate available tools) |
| 4 | tools/call browser_get_status |
| 5 | tools/call browser_list_tabs |
| 6 | ping |
| 7 | Invalid method error handling |
| 8 | DELETE session cleanup |

**Run:**
```bash
./tools/abp-mcp-test.sh
```

### Python Integration Tests

**Location:** `tools/abp-tests/`

Full end-to-end tests with result logging:

```bash
cd tools/abp-tests
./run_test.sh
```

Features:
- Launches Chrome with ABP automatically
- Navigates to external sites (Wikipedia, Google)
- Captures screenshots with element markup
- Logs results to SQLite database

## Test Pages

HTML test harnesses in `chrome/browser/abp/test_pages/`:

| File | Purpose |
|------|---------|
| `click-input-test.html` | Mouse clicks, keyboard input, form submission |
| `navigation-test.html` | Navigation between pages, history state |
| `navigation-test-page2.html` | Multi-page navigation target |
| `navigation-test-page3.html` | Multi-page navigation target |
| `screenshot-markup-test.html` | Element markup overlays (clickable, typeable, inputs) |
| `scroll-execution-test.html` | Scroll behavior, JS execution |
| `virtual-time-test.html` | Execution control, virtual time freezing |
| `dialog-file-test.html` | Alert/Confirm/Prompt dialogs, file upload/download |

### Test Assets

```
chrome/browser/abp/test_pages/assets/
├── test-download.pdf    # Download test file
├── test-upload.txt      # File chooser test file
└── test-image.png       # Image test file
```

## Test Output

### Integration Tests (run_tests.sh)

```
================================
ABP Integration Test Suite
================================

Checking prerequisites...
Prerequisites OK

Running tests...

Testing NAV-001 (Navigate to URL)... PASSED
Testing CLICK-001 (Click button)... PASSED
Testing TYPE-001 (Type text)... PASSED
Testing KEY-001 (Press Enter for form submit)... PASSED
Testing EXEC-001 (Execute JavaScript)... PASSED
Testing SCREENSHOT-001 (Take screenshot)... PASSED
Testing NAV-003 (Back/Forward navigation)... PASSED

--- Virtual Time Tests ---
Setting up virtual time test tab... OK
Testing VTIME-001 (Timer frozen when paused)... PASSED
Testing VTIME-002 (Date.now frozen when paused)... PASSED
Testing VTIME-003 (setTimeout not firing when paused)... PASSED

================================
Test Results
================================
Passed: 10
Failed: 0

All tests passed!
```

## Coverage

| Feature Category | Tested Endpoints |
|------------------|------------------|
| Tab Management | `/tabs`, `/tabs/{id}`, `POST /tabs` |
| Navigation | `/navigate`, `/back`, `/forward`, `/reload` |
| Mouse Input | `/click` |
| Keyboard Input | `/type`, `/keyboard/press` |
| Screenshots | `/screenshot` |
| JavaScript | `/execute` |
| Execution Control | `/execution` |
| MCP Protocol | `/mcp` (initialize, tools/list, tools/call, ping) |

## Adding New Tests

### Bash Test (run_tests.sh)

```bash
# 1. Add test function
test_new_001() {
    local tab_id=$(get_tab_id)
    
    # Navigate to test page
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/navigate" \
        -H "Content-Type: application/json" \
        -d '{"url":"'"$HTTP_URL"'/your-test-page.html"}' > /dev/null
    
    sleep 1
    
    # Verify result via JavaScript
    local result=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"window.testResult"}' | jq -r '.result.value')
    
    [[ "$result" == "expected_value" ]]
}

# 2. Register the test
run_test "NEW-001 (Description)" test_new_001
```

### Test Page Design Guidelines

Design test pages with:
- **Fixed CSS positions** for reliable coordinate-based clicking
- **`window.*` APIs** to expose state for verification
- **Clear visual indicators** for manual debugging

Example pattern from `click-input-test.html`:
```html
<!-- Fixed position button -->
<button id="btn-fixed" style="position: absolute; left: 50px; top: 80px;">
    Click Me
</button>

<script>
    // Expose state for test verification
    window.clickCount = 0;
    document.getElementById('btn-fixed').addEventListener('click', function() {
        window.clickCount++;
    });
</script>
```

## Troubleshooting

### Tests fail with "Connection refused"

Ensure Chrome is running with ABP enabled:
```bash
./out/Default/chrome --enable-abp --abp-port=8222
```

### Tests fail with "HTTP server not running"

Start the test page server:
```bash
cd chrome/browser/abp/test_pages
python3 -m http.server 8081
```

### Virtual time tests fail

Virtual time tests require execution control support. Ensure you're not running with `--abp-disable-pause`.

### jq not found

Install jq:
```bash
# Ubuntu/Debian
sudo apt install jq

# macOS
brew install jq
```
