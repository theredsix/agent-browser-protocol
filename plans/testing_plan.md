# ABP Integration Testing

**Status: IMPLEMENTED**

The test harness and test pages are fully implemented in `chrome/browser/abp/test_pages/`.

---

## Test Infrastructure

### Test Pages Location

```
chrome/browser/abp/test_pages/
├── run_tests.sh              # Bash test runner (10 test cases)
├── click-input-test.html     # Click, type, keyboard, form tests
├── navigation-test.html      # Navigation test page 1
├── navigation-test-page2.html # Navigation test page 2
├── navigation-test-page3.html # Navigation test page 3
├── screenshot-markup-test.html # Screenshot markup overlay tests
├── scroll-execution-test.html  # Scroll and execution tests
├── virtual-time-test.html    # Virtual time/execution control tests
├── dialog-file-test.html     # Dialog and file chooser tests
└── assets/
    ├── test-download.pdf     # Test file for download tests
    ├── test-upload.txt       # Test file for file chooser tests
    └── test-image.png        # Test image
```

### Running the Tests

```bash
# 1. Start Chrome with ABP enabled
./out/Default/chrome --enable-abp

# 2. Start HTTP server for test pages (in separate terminal)
cd chrome/browser/abp/test_pages
python3 -m http.server 8081

# 3. Run the test suite
./run_tests.sh
```

### Test Configuration

The test runner expects:
- ABP server running on `http://localhost:8222`
- HTTP server for test pages on `http://localhost:8081`

---

## Implemented Test Cases

The `run_tests.sh` script includes the following tests:

| Test ID | Description | Endpoint Tested |
|---------|-------------|-----------------|
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

### Virtual Time Tests

The virtual time tests (VTIME-*) verify that execution control properly freezes:
- `setInterval` timers
- `Date.now()` return values
- `setTimeout` callbacks

These tests create a dedicated tab with execution control enabled and verify that the page state remains frozen while paused.

---

## Test Page Specifications

### click-input-test.html

Tests mouse and keyboard input:
- Fixed-position click target at (100, 100)
- Click counter verification
- Text input field at (150, 195)
- Form with username field at (150, 495)
- Enter key form submission

### navigation-test.html (+ page2, page3)

Tests navigation controls:
- Page identification headers
- Links between pages
- History state verification

### virtual-time-test.html

Tests execution control:
- `setInterval` counter that increments every 100ms
- `Date.now()` display
- Scheduled timeout detection
- `window.virtualTimeTest` API for test queries

### screenshot-markup-test.html

Tests screenshot markup overlays:
- Interactive elements grid (buttons, links, inputs)
- Different markup categories (clickable, typeable, inputs)

### scroll-execution-test.html

Tests scrolling and JavaScript execution:
- Scrollable content (5000px height)
- Scroll position displays
- JavaScript execution verification

### dialog-file-test.html

Tests dialogs and file operations:
- Alert/Confirm/Prompt trigger buttons
- File input elements
- Download links

---

## Test Output

The test runner produces colored output:
- **GREEN**: Test passed
- **RED**: Test failed

Final summary shows:
```
================================
Test Results
================================
Passed: X
Failed: Y

All tests passed! (or) Some tests failed.
```

---

## Adding New Tests

To add a new test:

1. Add a test function in `run_tests.sh`:
```bash
test_new_001() {
    local tab_id=$(get_tab_id)
    # Your test implementation
    [[ "$result" == "expected" ]]
}
```

2. Register the test:
```bash
run_test "NEW-001 (Description)" test_new_001
```

---

## ABP Features Validated by Tests

| Feature Category | Endpoints Tested |
|------------------|------------------|
| Tab Management | `GET /tabs`, `POST /tabs/{id}/navigate` |
| Navigation | `back`, `forward`, `reload` |
| Mouse Input | `click` |
| Keyboard Input | `type`, `keyboard/press` |
| Screenshots | `screenshot` |
| JavaScript | `execute` |
| Execution Control | `execution` GET/POST |
