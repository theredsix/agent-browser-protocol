# ABP Integration Testing - Technical Specification

## 1. Overview

### 1.1 Purpose

This document specifies a comprehensive integration test suite for the Agent Browser Protocol (ABP) using static HTML/JS test pages. The tests validate that ABP REST endpoints correctly interact with web page elements and return accurate response data.

### 1.2 Goals

- Validate all ABP REST API endpoints against controlled test pages
- Ensure screenshot markup overlays correctly identify interactive elements
- Verify input actions (click, type, scroll) produce expected DOM changes
- Test navigation controls (back, forward, reload) and history tracking
- Validate dialog handling (alert, confirm, prompt)
- Test file chooser and download workflows
- Verify execution control (pause/resume) and virtual time behavior
- Confirm event collection captures all relevant browser events

### 1.3 ABP Features Validated

| Feature Category | Endpoints Tested |
|------------------|------------------|
| Tab Management | `GET /tabs`, `GET /tabs/{id}`, `POST /tabs`, `DELETE /tabs/{id}` |
| Navigation | `POST /tabs/{id}/navigate`, `POST /tabs/{id}/back`, `POST /tabs/{id}/forward`, `POST /tabs/{id}/reload` |
| Mouse Input | `POST /tabs/{id}/click`, `POST /tabs/{id}/move`, `POST /tabs/{id}/scroll` |
| Keyboard Input | `POST /tabs/{id}/type`, `POST /tabs/{id}/keyboard/press`, `POST /tabs/{id}/keyboard/down`, `POST /tabs/{id}/keyboard/up` |
| Screenshots | `GET /tabs/{id}/screenshot`, `POST /tabs/{id}/screenshot` |
| JavaScript | `POST /tabs/{id}/execute` |
| Dialogs | `GET /tabs/{id}/dialog`, `POST /tabs/{id}/dialog/accept`, `POST /tabs/{id}/dialog/dismiss` |
| Downloads | `GET /downloads`, `GET /downloads/{id}`, `POST /downloads/{id}/cancel` |
| File Chooser | `POST /file-chooser/{id}` |
| Execution Control | `GET /tabs/{id}/execution`, `POST /tabs/{id}/execution` |
| Browser Status | `GET /browser/status` |

---

## 2. Test Environment

### 2.1 Test File Serving

Test HTML files must be served via HTTP to enable full browser functionality (file:// URLs restrict certain APIs). Options:

**Option A: Built-in Python HTTP Server**
```bash
cd /path/to/test-pages
python3 -m http.server 8080
```

**Option B: ABP-Served Test Files**
Store test files in the session directory and serve via a dedicated endpoint:
```
GET /test-pages/{filename}
```

**Option C: Chrome Extension Test Server**
Use Chromium's existing test infrastructure if available.

**Recommendation**: Option A for simplicity during initial development; migrate to Option B if test page serving becomes a common need.

### 2.2 ABP Configuration Requirements

```bash
./out/Default/chrome \
  --enable-abp \
  --abp-port=8222 \
  --abp-session-dir=/tmp/abp-test-session \
  --window-size=1280,720 \
  --disable-extensions \
  --no-first-run
```

Required configuration:
- **Viewport size**: Fixed at 1280x720 for deterministic screenshot comparison
- **Execution control**: Enabled by default (tests rely on paused state between actions)
- **Session directory**: Unique per test run for isolation
- **Downloads path**: Set to session directory subdirectory

### 2.3 Directory Structure

```
chrome/browser/abp/test_pages/
├── click-input-test.html
├── navigation-test.html
├── screenshot-markup-test.html
├── dialog-file-test.html
├── scroll-execution-test.html
└── assets/
    ├── test-download.pdf        # 1KB test file for download tests
    ├── test-upload.txt          # Test file for file chooser tests
    └── test-image.png           # Test image for file chooser tests
```

### 2.4 Test Harness Requirements

The test harness (implementation not specified here) must:
1. Start ABP-enabled Chrome instance
2. Wait for `GET /browser/status` to return `ready: true`
3. Navigate to test page via `POST /tabs/{id}/navigate`
4. Execute test cases sequentially
5. Compare responses against expected values
6. Generate pass/fail report with failure details

---

## 3. Test Page Specifications

### 3.1 Click and Input Test Page (click-input-test.html)

#### 3.1.1 Purpose and Scope

Validates mouse click actions at specific coordinates, text input via keyboard, keyboard event handling, and form submission. This page tests the fundamental input mechanisms that AI agents rely on for web interaction.

#### 3.1.2 Required HTML Elements

**Click Target Section**
- A fixed-position button at coordinates (100, 100) with ID `btn-fixed` and text "Click Me"
- A counter display with ID `click-count` showing number of clicks (initially "0")
- A right-click target div at coordinates (300, 100) with ID `right-click-target`
- A context menu indicator with ID `context-menu-shown` (initially hidden)
- A double-click target at coordinates (500, 100) with ID `double-click-target`
- A double-click counter with ID `double-click-count`

**Text Input Section**
- A text input field at coordinates (100, 200) with ID `text-input` and placeholder "Type here"
- A textarea at coordinates (100, 300) with ID `text-area` (5 rows, 40 cols)
- A password field at coordinates (100, 400) with ID `password-input`
- A readonly input at coordinates (300, 200) with ID `readonly-input` containing "Cannot edit"
- A disabled input at coordinates (300, 300) with ID `disabled-input`

**Keyboard Events Section**
- A key event display with ID `last-key-event` showing last keydown/keyup event details
- A modifier state display with ID `modifier-state` showing current Shift/Ctrl/Alt/Meta state
- An input field with ID `key-event-input` that captures and displays key events

**Form Section**
- A form with ID `test-form` containing:
  - Text input with name `username`
  - Password input with name `password`
  - Submit button with ID `submit-btn` at coordinates (100, 550)
- A form submission indicator with ID `form-submitted` (initially hidden, shows "Form Submitted" after submission)

**Coordinate Display**
- A fixed position element with ID `mouse-coords` displaying current mouse position (updated on mousemove)

#### 3.1.3 Required JavaScript Behaviors

- Click counter increments on each click of `btn-fixed`
- Right-click on `right-click-target` shows context menu indicator (prevents default context menu)
- Double-click on `double-click-target` increments double-click counter
- All text inputs update their value on keypress
- `last-key-event` updates with JSON of last key event (`{key, code, keyCode, shiftKey, ctrlKey, altKey, metaKey}`)
- `modifier-state` updates in real-time showing which modifiers are pressed
- Form submission prevented by default, instead shows indicator and stores submitted values in `window.lastFormData`
- Mouse position tracked and displayed in `mouse-coords`

#### 3.1.4 Visual States for Screenshot Verification

| State ID | Description | Visual Indicator |
|----------|-------------|------------------|
| CLICK-INITIAL | Page loaded, no interactions | Click count shows "0", no highlights |
| CLICK-SINGLE | After single click on btn-fixed | Click count shows "1", button has :active style |
| CLICK-MULTIPLE | After 5 clicks | Click count shows "5" |
| CLICK-RIGHT | After right-click | Context menu indicator visible |
| CLICK-DOUBLE | After double-click | Double-click count incremented |
| INPUT-TYPED | After typing in text-input | Input shows typed text |
| INPUT-ENTER | After pressing Enter in form | Form submitted indicator visible |
| KEYS-SHIFT | While Shift held | Modifier state shows "Shift: true" |

#### 3.1.5 Test Case Specifications

---

**Test ID**: CLICK-001
**Description**: Single left click on fixed-position button
**Preconditions**: Page loaded, click count is 0
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 100,
  "y": 100,
  "button": "left",
  "click_count": 1,
  "screenshot": {"markup": "none"}
}
```
**Expected Response Fields**:
- `result.x`: 100
- `result.y`: 100
- `result.button`: "left"
- `screenshot.data`: non-empty base64 string
- `screenshot.width`: 1280
- `screenshot.height`: 720

**Screenshot Verification**: Click count display shows "1"
**Pass Criteria**: Response status 200, click count incremented, screenshot captured

---

**Test ID**: CLICK-002
**Description**: Right click shows context menu indicator
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 300,
  "y": 100,
  "button": "right",
  "click_count": 1
}
```
**Expected Response Fields**:
- `result.button`: "right"

**Screenshot Verification**: Context menu indicator element is visible
**Pass Criteria**: Context menu indicator displayed (verify via JS execution)

---

**Test ID**: CLICK-003
**Description**: Double click detection
**Preconditions**: Page loaded, double-click count is 0
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 500,
  "y": 100,
  "button": "left",
  "click_count": 2
}
```
**Expected Response Fields**:
- `result.x`: 500
- `result.y`: 500

**Screenshot Verification**: Double-click count shows "1"
**Pass Criteria**: Double-click event fired (verify via JS execution querying `double-click-count`)

---

**Test ID**: CLICK-004
**Description**: Click with Shift modifier
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 100,
  "y": 100,
  "button": "left",
  "modifiers": ["shift"]
}
```
**Expected Response Fields**:
- `result.x`: 100

**Pass Criteria**: Execute JS to verify `window.lastClickEvent.shiftKey === true`

---

**Test ID**: TYPE-001
**Description**: Type text into focused input field
**Preconditions**: Text input clicked/focused (CLICK on text-input coordinates first)
**ABP Endpoint**: `POST /tabs/{tab_id}/type`
**Request Body**:
```json
{
  "text": "Hello World"
}
```
**Expected Response Fields**:
- `result.text`: "Hello World"

**Screenshot Verification**: Text input displays "Hello World"
**Pass Criteria**: Execute JS: `document.getElementById('text-input').value === 'Hello World'`

---

**Test ID**: TYPE-002
**Description**: Type special characters
**Preconditions**: Text input focused
**ABP Endpoint**: `POST /tabs/{tab_id}/type`
**Request Body**:
```json
{
  "text": "Test@123!#$%"
}
```
**Pass Criteria**: Input value matches typed string exactly

---

**Test ID**: TYPE-003
**Description**: Type into textarea preserving newlines
**Preconditions**: Textarea focused
**ABP Endpoint**: `POST /tabs/{tab_id}/type`
**Request Body**:
```json
{
  "text": "Line 1\nLine 2\nLine 3"
}
```
**Pass Criteria**: Textarea contains three lines

---

**Test ID**: KEY-001
**Description**: Press Enter key
**Preconditions**: Form username field focused with text
**ABP Endpoint**: `POST /tabs/{tab_id}/keyboard/press`
**Request Body**:
```json
{
  "key": "Enter"
}
```
**Expected Response Fields**:
- `result.key`: "Enter"

**Pass Criteria**: Form submission indicator visible

---

**Test ID**: KEY-002
**Description**: Press Tab to change focus
**Preconditions**: Username field focused
**ABP Endpoint**: `POST /tabs/{tab_id}/keyboard/press`
**Request Body**:
```json
{
  "key": "Tab"
}
```
**Pass Criteria**: Execute JS to verify `document.activeElement.name === 'password'`

---

**Test ID**: KEY-003
**Description**: Keyboard shortcut Ctrl+A (select all)
**Preconditions**: Text input with content focused
**ABP Endpoint**: `POST /tabs/{tab_id}/keyboard/press`
**Request Body**:
```json
{
  "key": "a",
  "modifiers": ["Control"]
}
```
**Pass Criteria**: Execute JS to verify text is selected (selectionStart=0, selectionEnd=text.length)

---

**Test ID**: KEY-004
**Description**: Key down/up for held modifier
**Preconditions**: Page loaded
**ABP Endpoints**:
1. `POST /tabs/{tab_id}/keyboard/down` with `{"key": "Shift"}`
2. `POST /tabs/{tab_id}/keyboard/up` with `{"key": "Shift"}`

**Pass Criteria**: Modifier state shows Shift:true after down, Shift:false after up

---

**Test ID**: KEY-005
**Description**: Arrow key navigation
**Preconditions**: Text input with "Hello" focused, cursor at end
**ABP Endpoint**: `POST /tabs/{tab_id}/keyboard/press`
**Request Body**:
```json
{
  "key": "ArrowLeft"
}
```
**Pass Criteria**: Cursor position moved (verify via selectionStart)

---

**Test ID**: FORM-001
**Description**: Complete form submission workflow
**Preconditions**: Page loaded
**ABP Sequence**:
1. Click username field (100, 500)
2. Type "testuser"
3. Press Tab
4. Type "password123"
5. Click submit button (100, 550)

**Pass Criteria**: Form submitted indicator visible, `window.lastFormData` contains correct values

---

### 3.2 Navigation Test Page (navigation-test.html)

#### 3.2.1 Purpose and Scope

Validates URL navigation, browser history controls (back/forward), page reload, and navigation event capture. Tests that navigation actions correctly update tab state and emit appropriate events.

#### 3.2.2 Required HTML Elements

**Page Identification**
- A large heading with ID `page-title` displaying "Navigation Test - Page 1" (or 2, 3 depending on page)
- A unique page identifier div with ID `page-id` containing a UUID or page number
- A navigation history display with ID `nav-history` showing pages visited

**Navigation Links**
- Link to "Page 2" with ID `link-page-2` at coordinates (100, 150)
- Link to "Page 3" with ID `link-page-3` at coordinates (100, 200)
- Link to external site (example.com) with ID `link-external` at coordinates (100, 250)
- Link that opens in new tab (`target="_blank"`) with ID `link-new-tab` at coordinates (100, 300)

**History State Section**
- Display of `window.history.length` with ID `history-length`
- Display of current `window.location.href` with ID `current-url`
- Display of `document.referrer` with ID `referrer`

**Reload Detection**
- A timestamp display with ID `load-timestamp` showing page load time
- A visit counter with ID `visit-count` stored in sessionStorage (increments on each load)
- A reload indicator with ID `was-reloaded` that detects `performance.navigation.type`

**Page 2 and Page 3**
- Separate HTML files (navigation-test-page2.html, navigation-test-page3.html) with:
  - Same structure but different page-id and page-title
  - Back link to Page 1 with ID `link-back`
  - Navigation timestamp for timing verification

#### 3.2.3 Required JavaScript Behaviors

- On page load, record timestamp in `load-timestamp`
- Increment and display `visit-count` from sessionStorage
- Detect if page was reloaded vs fresh navigation
- Update `history-length` display
- Store visited pages in sessionStorage for `nav-history` display
- All links work normally (no preventDefault)

#### 3.2.4 Visual States for Screenshot Verification

| State ID | Description | Visual Indicator |
|----------|-------------|------------------|
| NAV-PAGE1 | Initial page 1 | Title shows "Page 1", visit count "1" |
| NAV-PAGE2 | After navigating to page 2 | Title shows "Page 2" |
| NAV-BACK | After going back to page 1 | Title shows "Page 1", visit count "2" |
| NAV-FORWARD | After going forward to page 2 | Title shows "Page 2" |
| NAV-RELOAD | After reload | Same title, visit count incremented, reload indicator true |

#### 3.2.5 Test Case Specifications

---

**Test ID**: NAV-001
**Description**: Navigate to URL via endpoint
**Preconditions**: Fresh tab
**ABP Endpoint**: `POST /tabs/{tab_id}/navigate`
**Request Body**:
```json
{
  "url": "http://localhost:8080/navigation-test.html",
  "wait_until": {"type": "action_complete"}
}
```
**Expected Response Fields**:
- `result.url`: contains "navigation-test.html"
- `result.title`: "Navigation Test - Page 1"
- `events`: contains navigation event with `navigation_type: "typed"` or similar

**Screenshot Verification**: Page 1 title visible
**Pass Criteria**: Tab URL updated, page loaded, navigation event captured

---

**Test ID**: NAV-002
**Description**: Navigate via link click
**Preconditions**: On page 1
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 100,
  "y": 150
}
```
**Expected Response Fields**:
- `events`: contains navigation event with `navigation_type: "link_click"`
- `events[].data.url`: contains "page2"

**Screenshot Verification**: Page 2 title visible
**Pass Criteria**: Navigation event captured, page 2 loaded

---

**Test ID**: NAV-003
**Description**: Go back in history
**Preconditions**: Navigated from page 1 to page 2
**ABP Endpoint**: `POST /tabs/{tab_id}/back`
**Request Body**:
```json
{
  "wait_until": {"type": "action_complete"}
}
```
**Expected Response Fields**:
- `events`: contains navigation event with `navigation_type: "back_forward"`

**Screenshot Verification**: Page 1 title visible, visit count incremented
**Pass Criteria**: URL is page 1, navigation event type is back_forward

---

**Test ID**: NAV-004
**Description**: Go forward in history
**Preconditions**: On page 1 after going back from page 2
**ABP Endpoint**: `POST /tabs/{tab_id}/forward`
**Request Body**:
```json
{
  "wait_until": {"type": "action_complete"}
}
```
**Expected Response Fields**:
- `events`: contains navigation event with `navigation_type: "back_forward"`

**Screenshot Verification**: Page 2 title visible
**Pass Criteria**: URL is page 2

---

**Test ID**: NAV-005
**Description**: Reload page
**Preconditions**: On page 1
**ABP Endpoint**: `POST /tabs/{tab_id}/reload`
**Request Body**:
```json
{
  "ignore_cache": false,
  "wait_until": {"type": "action_complete"}
}
```
**Expected Response Fields**:
- `events`: contains navigation event with `navigation_type: "reload"`

**Pass Criteria**: Visit count incremented, reload indicator true (verify via JS)

---

**Test ID**: NAV-006
**Description**: Reload ignoring cache
**Preconditions**: On page 1
**ABP Endpoint**: `POST /tabs/{tab_id}/reload`
**Request Body**:
```json
{
  "ignore_cache": true
}
```
**Pass Criteria**: Page reloaded, visit count incremented

---

**Test ID**: NAV-007
**Description**: Navigate to URL with referrer
**Preconditions**: Fresh tab
**ABP Endpoint**: `POST /tabs/{tab_id}/navigate`
**Request Body**:
```json
{
  "url": "http://localhost:8080/navigation-test.html",
  "referrer": "https://google.com"
}
```
**Pass Criteria**: Execute JS to verify `document.referrer === 'https://google.com/'`

---

**Test ID**: NAV-008
**Description**: Get tab info after navigation
**Preconditions**: Navigated to page 1
**ABP Endpoint**: `GET /tabs/{tab_id}`
**Expected Response Fields**:
- `id`: tab_id
- `url`: contains "navigation-test.html"
- `title`: "Navigation Test - Page 1"
- `loading`: false

**Pass Criteria**: All fields present and accurate

---

**Test ID**: NAV-009
**Description**: Link click opens new tab (popup event)
**Preconditions**: On page 1
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 100,
  "y": 300
}
```
**Expected Response Fields**:
- `events`: contains popup event with `popup_type: "tab"`
- `events[].data.new_tab_id`: non-empty string

**Pass Criteria**: Popup event captured, new tab ID returned

---

**Test ID**: NAV-010
**Description**: Stop loading during navigation
**Preconditions**: Start slow-loading navigation
**ABP Sequence**:
1. Navigate to slow-loading resource
2. Immediately call `POST /tabs/{tab_id}/stop`

**Pass Criteria**: Loading stops, no timeout error

---

### 3.3 Screenshot Markup Test Page (screenshot-markup-test.html)

#### 3.3.1 Purpose and Scope

Validates screenshot capture functionality and element markup overlays. Tests that different markup modes correctly identify and annotate interactive elements, and that screenshot options (format, cursor) work correctly.

#### 3.3.2 Required HTML Elements

**Interactive Elements Grid** (arranged in predictable positions for coordinate testing)

Row 1 (y=100): Clickable elements
- Button with ID `btn-primary` at x=100, text "Primary Button"
- Link with ID `link-standard` at x=300, text "Standard Link"
- Button with ID `btn-secondary` at x=500, text "Secondary Button"
- `<div onclick>` with ID `div-clickable` at x=700, text "Clickable Div"

Row 2 (y=200): Typeable elements
- Text input with ID `input-text` at x=100
- Textarea with ID `textarea-main` at x=300
- Contenteditable div with ID `div-editable` at x=500
- Password input with ID `input-password` at x=700

Row 3 (y=300): Form inputs
- Checkbox with ID `checkbox-1` at x=100
- Radio button with ID `radio-1` at x=200
- Select dropdown with ID `select-main` at x=300
- Range slider with ID `range-slider` at x=500
- Date input with ID `input-date` at x=700

Row 4 (y=400): Non-interactive elements (should NOT be marked)
- Static div with ID `div-static` at x=100
- Disabled button with ID `btn-disabled` at x=300
- Hidden input with ID `input-hidden` at x=500
- Span with ID `span-text` at x=700

Row 5 (y=500): Edge cases
- Button inside link (nested interactive) with ID `nested-btn` at x=100
- Zero-size element with ID `zero-size` at x=300
- Overflow hidden container with partially visible button with ID `btn-overflow` at x=500
- Absolutely positioned element with ID `abs-positioned` at x=700

**Markup Legend**
- Color reference showing expected markup colors for each element type

#### 3.3.3 Required JavaScript Behaviors

- No special behaviors required - this page tests screenshot capture
- Optional: Add a counter that increments every 100ms to test execution pause freezes animations

#### 3.3.4 Visual States for Screenshot Verification

| State ID | Description | Visual Indicator |
|----------|-------------|------------------|
| MARKUP-NONE | No markup | Clean page, no overlays |
| MARKUP-INTERACTIVE | All interactive elements | Boxes around rows 1-3 elements |
| MARKUP-CLICKABLE | Only clickable elements | Boxes around row 1 elements only |
| MARKUP-TYPEABLE | Only typeable elements | Boxes around row 2 elements only |
| MARKUP-INPUTS | All form inputs | Boxes around rows 2-3 elements |
| CURSOR-VISIBLE | Cursor in screenshot | Virtual cursor rendered at specific position |
| CURSOR-HIDDEN | Cursor hidden | No cursor in screenshot |

#### 3.3.5 Test Case Specifications

---

**Test ID**: SHOT-001
**Description**: Screenshot without markup
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/screenshot`
**Request Body**:
```json
{
  "screenshot": {
    "area": "viewport",
    "markup": "none"
  }
}
```
**Expected Response Fields**:
- `screenshot.data`: non-empty base64 string
- `screenshot.format`: "webp"
- `screenshot.width`: 1280
- `screenshot.height`: 720

**Screenshot Verification**: No colored bounding boxes visible
**Pass Criteria**: Valid WebP image returned at expected dimensions

---

**Test ID**: SHOT-002
**Description**: Screenshot with interactive markup
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/screenshot`
**Request Body**:
```json
{
  "screenshot": {
    "area": "viewport",
    "markup": "interactive"
  }
}
```
**Screenshot Verification**: Colored boxes around buttons, links, inputs, textareas, contenteditable
**Pass Criteria**: All interactive elements in rows 1-3 have markup overlays

---

**Test ID**: SHOT-003
**Description**: Screenshot with clickable-only markup
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/screenshot`
**Request Body**:
```json
{
  "screenshot": {
    "markup": "clickable"
  }
}
```
**Screenshot Verification**: Boxes only around buttons and links (row 1)
**Pass Criteria**: Inputs and textareas do NOT have markup

---

**Test ID**: SHOT-004
**Description**: Screenshot with typeable-only markup
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/screenshot`
**Request Body**:
```json
{
  "screenshot": {
    "markup": "typeable"
  }
}
```
**Screenshot Verification**: Boxes only around text inputs, textarea, contenteditable (row 2)
**Pass Criteria**: Buttons and links do NOT have markup

---

**Test ID**: SHOT-005
**Description**: Screenshot with inputs markup
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/screenshot`
**Request Body**:
```json
{
  "screenshot": {
    "markup": "inputs"
  }
}
```
**Screenshot Verification**: Boxes around all form inputs (rows 2-3)
**Pass Criteria**: Checkboxes, radios, selects, sliders all have markup

---

**Test ID**: SHOT-006
**Description**: Screenshot with cursor visible
**Preconditions**: Mouse moved to (400, 300)
**ABP Sequence**:
1. `POST /tabs/{tab_id}/move` with `{"x": 400, "y": 300}`
2. `POST /tabs/{tab_id}/screenshot` with `{"screenshot": {"cursor": true}}`

**Screenshot Verification**: Cursor visible at (400, 300)
**Pass Criteria**: Virtual cursor rendered in screenshot

---

**Test ID**: SHOT-007
**Description**: Screenshot with cursor hidden
**Preconditions**: Mouse at (400, 300)
**ABP Endpoint**: `POST /tabs/{tab_id}/screenshot`
**Request Body**:
```json
{
  "screenshot": {
    "cursor": false
  }
}
```
**Screenshot Verification**: No cursor visible
**Pass Criteria**: Screenshot does not contain virtual cursor

---

**Test ID**: SHOT-008
**Description**: Binary screenshot via GET endpoint
**Preconditions**: Page loaded
**ABP Endpoint**: `GET /tabs/{tab_id}/screenshot`
**Expected Response**:
- Content-Type: `image/webp`
- Body: Raw WebP binary data

**Pass Criteria**: Valid WebP image data returned

---

**Test ID**: SHOT-009
**Description**: Markup does not include disabled elements
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/screenshot`
**Request Body**:
```json
{
  "screenshot": {
    "markup": "interactive"
  }
}
```
**Screenshot Verification**: No box around `btn-disabled`
**Pass Criteria**: Disabled button excluded from markup

---

**Test ID**: SHOT-010
**Description**: Markup does not include hidden elements
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/screenshot`
**Request Body**:
```json
{
  "screenshot": {
    "markup": "inputs"
  }
}
```
**Screenshot Verification**: No box at `input-hidden` location
**Pass Criteria**: Hidden input excluded from markup

---

### 3.4 Dialog and File Test Page (dialog-file-test.html)

#### 3.4.1 Purpose and Scope

Validates browser dialog handling (alert, confirm, prompt, beforeunload) and file operations (file chooser, downloads). Tests that dialogs are correctly detected, can be accepted/dismissed, and that file chooser events are properly captured.

#### 3.4.2 Required HTML Elements

**Dialog Trigger Section** (y=100-200)
- Button "Show Alert" with ID `btn-alert` at x=100, triggers `alert('Test alert message')`
- Button "Show Confirm" with ID `btn-confirm` at x=250, triggers `confirm('Confirm this action?')`
- Button "Show Prompt" with ID `btn-prompt` at x=400, triggers `prompt('Enter your name:', 'Default')`
- Checkbox "Enable beforeunload" with ID `chk-beforeunload` at x=550

**Dialog Result Display**
- Div with ID `alert-result` showing "Alert was shown" after alert dismissed
- Div with ID `confirm-result` showing "Confirmed: true/false"
- Div with ID `prompt-result` showing "Entered: {value}" or "Cancelled"

**File Upload Section** (y=300-400)
- File input (single) with ID `file-single` at x=100, accepts all files
- File input (multiple) with ID `file-multiple` at x=300, accepts multiple files
- File input (images only) with ID `file-images` at x=500, accepts="image/*"
- File drop zone div with ID `drop-zone` at x=100, y=350, 200x100px

**File Selection Display**
- Div with ID `files-selected` listing selected file names
- Div with ID `file-details` showing size, type of first selected file

**Download Section** (y=500-600)
- Link to download test PDF with ID `download-link` at x=100
- Button to trigger programmatic download with ID `btn-download` at x=300
- Download progress indicator with ID `download-progress`
- Download complete indicator with ID `download-complete`

**Save File Section**
- Button "Save Text File" with ID `btn-save` at x=500, triggers download with custom filename

#### 3.4.3 Required JavaScript Behaviors

- Alert button: Shows alert, then updates `alert-result`
- Confirm button: Shows confirm, stores result in `window.confirmResult`, updates `confirm-result`
- Prompt button: Shows prompt, stores result in `window.promptResult`, updates `prompt-result`
- beforeunload checkbox: When checked, adds beforeunload handler
- File inputs: On change, update `files-selected` with file names, `file-details` with first file info
- Drop zone: Handle dragover/drop events, update file display
- Download link: Standard download link to test file
- Download button: Uses Blob API to create and download file
- Save button: Creates download with suggested filename

#### 3.4.4 Visual States for Screenshot Verification

| State ID | Description | Visual Indicator |
|----------|-------------|------------------|
| DLG-INITIAL | Page loaded | No dialogs, no results shown |
| DLG-ALERT | Alert shown | Screenshot captures alert dialog |
| DLG-CONFIRM | Confirm shown | Screenshot captures confirm dialog |
| DLG-PROMPT | Prompt shown | Screenshot captures prompt dialog with default value |
| DLG-RESULT | After dialog handled | Result div shows outcome |
| FILE-SELECTED | File selected | files-selected shows filename |
| DL-PROGRESS | Download in progress | Progress indicator visible |
| DL-COMPLETE | Download complete | Complete indicator visible |

#### 3.4.5 Test Case Specifications

---

**Test ID**: DLG-001
**Description**: Trigger and detect alert dialog
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 100,
  "y": 100
}
```
**Expected Response Fields**:
- `events`: contains dialog event with `dialog_type: "alert"`
- `events[].data.message`: "Test alert message"
- `events[].data.pending`: true

**Pass Criteria**: Dialog event captured with correct type and message

---

**Test ID**: DLG-002
**Description**: Get pending dialog info
**Preconditions**: Alert dialog triggered (DLG-001)
**ABP Endpoint**: `GET /tabs/{tab_id}/dialog`
**Expected Response Fields**:
- `present`: true
- `type`: "alert"
- `message`: "Test alert message"
- `default_prompt`: ""

**Pass Criteria**: All dialog fields returned correctly

---

**Test ID**: DLG-003
**Description**: Accept alert dialog
**Preconditions**: Alert dialog open
**ABP Endpoint**: `POST /tabs/{tab_id}/dialog/accept`
**Request Body**:
```json
{}
```
**Pass Criteria**: Dialog dismissed, `alert-result` shows "Alert was shown"

---

**Test ID**: DLG-004
**Description**: Trigger and accept confirm dialog
**Preconditions**: Page loaded
**ABP Sequence**:
1. Click confirm button at (250, 100)
2. Accept dialog

**Pass Criteria**: `confirm-result` shows "Confirmed: true", `window.confirmResult === true`

---

**Test ID**: DLG-005
**Description**: Dismiss confirm dialog
**Preconditions**: Confirm dialog open
**ABP Endpoint**: `POST /tabs/{tab_id}/dialog/dismiss`
**Pass Criteria**: `confirm-result` shows "Confirmed: false", `window.confirmResult === false`

---

**Test ID**: DLG-006
**Description**: Accept prompt with custom text
**Preconditions**: Prompt dialog open
**ABP Endpoint**: `POST /tabs/{tab_id}/dialog/accept`
**Request Body**:
```json
{
  "prompt_text": "Custom Input"
}
```
**Pass Criteria**: `prompt-result` shows "Entered: Custom Input"

---

**Test ID**: DLG-007
**Description**: Dismiss prompt dialog
**Preconditions**: Prompt dialog open
**ABP Endpoint**: `POST /tabs/{tab_id}/dialog/dismiss`
**Pass Criteria**: `prompt-result` shows "Cancelled", `window.promptResult === null`

---

**Test ID**: DLG-008
**Description**: No dialog returns present: false
**Preconditions**: No dialog open
**ABP Endpoint**: `GET /tabs/{tab_id}/dialog`
**Expected Response Fields**:
- `present`: false

**Pass Criteria**: Correctly reports no pending dialog

---

**Test ID**: FILE-001
**Description**: Click file input triggers file chooser event
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 100,
  "y": 300
}
```
**Expected Response Fields**:
- `events`: contains file_chooser event
- `events[].data.id`: non-empty chooser ID
- `events[].data.chooser_type`: "open"
- `events[].data.multiple`: false
- `events[].data.pending`: true

**Pass Criteria**: File chooser event captured with unique ID

---

**Test ID**: FILE-002
**Description**: Provide file to file chooser
**Preconditions**: File chooser triggered (FILE-001)
**ABP Endpoint**: `POST /file-chooser/{chooser_id}`
**Request Body**:
```json
{
  "files": ["/path/to/test-upload.txt"]
}
```
**Expected Response Fields**:
- `id`: chooser_id
- `files_provided`: ["/path/to/test-upload.txt"]
- `closed`: true

**Pass Criteria**: File selected, `files-selected` displays filename

---

**Test ID**: FILE-003
**Description**: Cancel file chooser
**Preconditions**: File chooser triggered
**ABP Endpoint**: `POST /file-chooser/{chooser_id}`
**Request Body**:
```json
{
  "cancel": true
}
```
**Expected Response Fields**:
- `closed`: true

**Pass Criteria**: File chooser dismissed, no file selected

---

**Test ID**: FILE-004
**Description**: Multiple file selection
**Preconditions**: Multi-file input clicked
**ABP Endpoint**: `POST /file-chooser/{chooser_id}`
**Request Body**:
```json
{
  "files": ["/path/to/file1.txt", "/path/to/file2.txt"]
}
```
**Pass Criteria**: Both files appear in `files-selected`

---

**Test ID**: FILE-005
**Description**: Invalid chooser ID returns error
**Preconditions**: None
**ABP Endpoint**: `POST /file-chooser/invalid-id`
**Request Body**:
```json
{
  "files": ["/path/to/file.txt"]
}
```
**Expected Response**:
- Status: 404
- `error`: contains "not found"

**Pass Criteria**: Appropriate error returned

---

**Test ID**: DL-001
**Description**: Click download link starts download
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 100,
  "y": 500
}
```
**Expected Response Fields**:
- `events`: contains download_started event
- `events[].data.download_id`: non-empty
- `events[].data.filename`: "test-download.pdf"

**Pass Criteria**: Download started event captured

---

**Test ID**: DL-002
**Description**: List downloads
**Preconditions**: Download in progress or completed
**ABP Endpoint**: `GET /downloads`
**Expected Response Fields**:
- Array containing download object with `id`, `filename`, `state`

**Pass Criteria**: Download appears in list

---

**Test ID**: DL-003
**Description**: Get download status
**Preconditions**: Download started
**ABP Endpoint**: `GET /downloads/{download_id}`
**Expected Response Fields**:
- `id`: download_id
- `state`: "in_progress" or "completed"
- `bytes_received`: number
- `filename`: "test-download.pdf"

**Pass Criteria**: All download fields present

---

**Test ID**: DL-004
**Description**: Download completed event
**Preconditions**: Download in progress
**ABP Action**: Wait for download completion
**Expected Response Fields**:
- `events`: contains download_completed event
- `events[].data.path`: path to downloaded file

**Pass Criteria**: Download file exists at reported path

---

**Test ID**: DL-005
**Description**: Cancel in-progress download
**Preconditions**: Large download in progress
**ABP Endpoint**: `POST /downloads/{download_id}/cancel`
**Expected Response Fields**:
- `id`: download_id
- `state`: "cancelled"

**Pass Criteria**: Download state is cancelled

---

### 3.5 Scroll and Execution Test Page (scroll-execution-test.html)

#### 3.5.1 Purpose and Scope

Validates scroll actions, virtual cursor tracking across scroll positions, JavaScript execution, and execution control (pause/resume). Tests the deterministic page state behavior between agent actions.

#### 3.5.2 Required HTML Elements

**Scrollable Content**
- Page height of 5000px to enable significant scrolling
- Fixed header with ID `fixed-header` at top showing current scroll position
- Section markers every 1000px with IDs `section-1` through `section-5`
- Each section contains a unique identifier text and coordinates display

**Scroll Position Display**
- Div with ID `scroll-y` showing current `window.scrollY`
- Div with ID `scroll-percent` showing percentage scrolled
- Div with ID `viewport-info` showing viewport dimensions

**Interactive Elements at Various Scroll Positions**
- Button at top (y=100) with ID `btn-top`
- Button at middle (y=2500) with ID `btn-middle`
- Button at bottom (y=4900) with ID `btn-bottom`
- Each button shows "Clicked" text when clicked

**Time-Based Elements**
- Clock display with ID `clock` updated every 100ms showing `Date.now()`
- Animation div with ID `animated` that moves continuously via CSS animation
- Timer counter with ID `timer-count` incremented by setInterval

**Execution State Display**
- Div with ID `last-js-result` showing result of last executed script
- Div with ID `execution-log` appending each script execution timestamp

**Virtual Cursor Test Area**
- Large div with ID `cursor-area` at y=1500, 500x500px
- Shows current virtual cursor position when mouse moves over it

#### 3.5.3 Required JavaScript Behaviors

- Update `scroll-y` and `scroll-percent` on scroll events
- Increment `timer-count` every 100ms via setInterval
- Update `clock` every 100ms with `Date.now()`
- CSS animation runs continuously on `animated` div
- Store all executed scripts and results in `window.executionHistory` array
- Cursor area tracks mousemove and displays coordinates

#### 3.5.4 Visual States for Screenshot Verification

| State ID | Description | Visual Indicator |
|----------|-------------|------------------|
| SCROLL-TOP | At top of page | scroll-y shows "0", section-1 visible |
| SCROLL-MID | Scrolled to middle | scroll-y shows ~2500, section-3 visible |
| SCROLL-BOTTOM | Scrolled to bottom | scroll-y shows ~4280, section-5 visible |
| EXEC-PAUSED | Execution paused | Clock frozen, timer stopped, animation paused |
| EXEC-RESUMED | Execution resumed | Clock updating, timer incrementing |
| CURSOR-TRACKED | After mouse move | Cursor position shown in cursor-area |

#### 3.5.5 Test Case Specifications

---

**Test ID**: SCROLL-001
**Description**: Scroll down via wheel event
**Preconditions**: Page loaded at top
**ABP Endpoint**: `POST /tabs/{tab_id}/scroll`
**Request Body**:
```json
{
  "x": 640,
  "y": 360,
  "delta_x": 0,
  "delta_y": -500
}
```
**Expected Response Fields**:
- `scroll.vertical_px`: approximately 500
- `scroll.vertical_percent`: > 0
- `events`: contains scroll event with `delta.direction: "down"`

**Pass Criteria**: Page scrolled down, scroll position updated

---

**Test ID**: SCROLL-002
**Description**: Scroll up via wheel event
**Preconditions**: Scrolled down
**ABP Endpoint**: `POST /tabs/{tab_id}/scroll`
**Request Body**:
```json
{
  "x": 640,
  "y": 360,
  "delta_x": 0,
  "delta_y": 300
}
```
**Expected Response Fields**:
- `events`: contains scroll event with `delta.direction: "up"`

**Pass Criteria**: Page scrolled up, vertical_px decreased

---

**Test ID**: SCROLL-003
**Description**: Scroll position in response envelope
**Preconditions**: Scrolled to middle
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 100,
  "y": 100
}
```
**Expected Response Fields**:
- `scroll.horizontal_percent`: 0
- `scroll.vertical_percent`: > 0
- `scroll.page_height`: approximately 5000
- `scroll.viewport_height`: 720

**Pass Criteria**: Scroll position accurately reported in every action response

---

**Test ID**: SCROLL-004
**Description**: Click element at scrolled position
**Preconditions**: Scrolled so btn-middle is visible
**ABP Sequence**:
1. Scroll down to y=2500 region
2. Click btn-middle

**Pass Criteria**: Button click registered (verify via JS)

---

**Test ID**: SCROLL-005
**Description**: Virtual cursor position after scroll
**Preconditions**: Cursor at (400, 300), then scroll down
**ABP Sequence**:
1. Move to (400, 300)
2. Scroll down 1000px
3. Take screenshot with cursor

**Screenshot Verification**: Cursor still appears at viewport-relative (400, 300)
**Pass Criteria**: Cursor rendered at correct viewport position

---

**Test ID**: EXEC-001
**Description**: Execute JavaScript expression
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/execute`
**Request Body**:
```json
{
  "expression": "document.getElementById('scroll-y').textContent"
}
```
**Expected Response Fields**:
- `result.value`: "0" (or current scroll position)
- `result.type`: "string"

**Pass Criteria**: Correct value returned

---

**Test ID**: EXEC-002
**Description**: Execute JavaScript returning number
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/execute`
**Request Body**:
```json
{
  "expression": "window.scrollY"
}
```
**Expected Response Fields**:
- `result.value`: 0
- `result.type`: "number"

**Pass Criteria**: Number type correctly identified

---

**Test ID**: EXEC-003
**Description**: Execute JavaScript returning object
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/execute`
**Request Body**:
```json
{
  "expression": "({name: 'test', count: 42})"
}
```
**Expected Response Fields**:
- `result.value.name`: "test"
- `result.value.count`: 42
- `result.type`: "object"

**Pass Criteria**: Object serialized correctly

---

**Test ID**: EXEC-004
**Description**: Execute async/promise expression
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/execute`
**Request Body**:
```json
{
  "expression": "new Promise(r => setTimeout(() => r('delayed'), 100))",
  "await_promise": true,
  "timeout_ms": 5000
}
```
**Expected Response Fields**:
- `result.value`: "delayed"

**Pass Criteria**: Promise resolved and value returned

---

**Test ID**: EXEC-005
**Description**: JavaScript execution timeout
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/execute`
**Request Body**:
```json
{
  "expression": "new Promise(r => setTimeout(() => r('never'), 10000))",
  "await_promise": true,
  "timeout_ms": 500
}
```
**Expected Response**:
- Status: 500 or timeout error

**Pass Criteria**: Timeout error returned, not hung

---

**Test ID**: EXEC-006
**Description**: JavaScript evaluation error
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/execute`
**Request Body**:
```json
{
  "expression": "nonExistentVariable.property"
}
```
**Expected Response**:
- Status: 500
- `error`: contains "ReferenceError" or similar

**Pass Criteria**: Error message returned

---

**Test ID**: PAUSE-001
**Description**: Get execution state (paused by default)
**Preconditions**: Page loaded, execution control enabled
**ABP Endpoint**: `GET /tabs/{tab_id}/execution`
**Expected Response Fields**:
- `enabled`: true
- `paused`: true
- `virtual_time_base_ms`: number

**Pass Criteria**: Execution state correctly reported

---

**Test ID**: PAUSE-002
**Description**: Verify page frozen between actions
**Preconditions**: Page loaded
**ABP Sequence**:
1. Execute JS to get `timer-count` value (e.g., 10)
2. Wait 2 seconds (real time)
3. Execute JS to get `timer-count` value again

**Pass Criteria**: Timer count unchanged (page was frozen)

---

**Test ID**: PAUSE-003
**Description**: Verify clock frozen (virtual time)
**Preconditions**: Page loaded
**ABP Sequence**:
1. Execute JS to get `Date.now()` value
2. Wait 1 second (real time)
3. Execute JS to get `Date.now()` value again

**Pass Criteria**: `Date.now()` values are identical (virtual time frozen)

---

**Test ID**: PAUSE-004
**Description**: Resume execution manually
**Preconditions**: Execution paused
**ABP Endpoint**: `POST /tabs/{tab_id}/execution`
**Request Body**:
```json
{
  "paused": false
}
```
**Expected Response Fields**:
- `paused`: false

**Pass Criteria**: Timer starts incrementing (verify via delayed JS execution)

---

**Test ID**: PAUSE-005
**Description**: Pause execution manually
**Preconditions**: Execution resumed
**ABP Endpoint**: `POST /tabs/{tab_id}/execution`
**Request Body**:
```json
{
  "paused": true
}
```
**Expected Response Fields**:
- `paused`: true

**Pass Criteria**: Timer stops incrementing

---

**Test ID**: PAUSE-006
**Description**: Virtual time in screenshot response
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/screenshot`
**Expected Response Fields**:
- `screenshot.virtual_time_ms`: number (milliseconds since epoch)

**Pass Criteria**: Virtual time included in response

---

**Test ID**: WAIT-001
**Description**: Wait until action_complete
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 100,
  "y": 100,
  "wait_until": {
    "type": "action_complete",
    "timeout_ms": 5000
  }
}
```
**Expected Response Fields**:
- `timing.wait_completed_ms`: > `timing.action_completed_ms`

**Pass Criteria**: Response includes timing information

---

**Test ID**: WAIT-002
**Description**: Wait with immediate return
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 100,
  "y": 100,
  "wait_until": {
    "type": "immediate"
  }
}
```
**Expected Response Fields**:
- `timing.duration_ms`: < 100 (very fast)

**Pass Criteria**: Returns immediately without waiting

---

**Test ID**: WAIT-003
**Description**: Wait for fixed time
**Preconditions**: Page loaded
**ABP Endpoint**: `POST /tabs/{tab_id}/click`
**Request Body**:
```json
{
  "x": 100,
  "y": 100,
  "wait_until": {
    "type": "time",
    "duration_ms": 1000
  }
}
```
**Expected Response Fields**:
- `timing.duration_ms`: approximately 1000

**Pass Criteria**: Wait duration respected

---

---

## 4. Test Case Matrix

| Test ID | Endpoint | Feature | Screenshot Test | Event Test |
|---------|----------|---------|-----------------|------------|
| CLICK-001 | POST /click | Single click | Yes | No |
| CLICK-002 | POST /click | Right click | Yes | No |
| CLICK-003 | POST /click | Double click | No | No |
| CLICK-004 | POST /click | Modifier keys | No | No |
| TYPE-001 | POST /type | Text input | Yes | No |
| TYPE-002 | POST /type | Special chars | No | No |
| TYPE-003 | POST /type | Multiline | No | No |
| KEY-001 | POST /keyboard/press | Enter key | Yes | No |
| KEY-002 | POST /keyboard/press | Tab focus | No | No |
| KEY-003 | POST /keyboard/press | Shortcuts | No | No |
| KEY-004 | POST /keyboard/down,up | Held keys | No | No |
| KEY-005 | POST /keyboard/press | Arrow keys | No | No |
| FORM-001 | Multiple | Form workflow | Yes | No |
| NAV-001 | POST /navigate | URL navigation | Yes | Yes |
| NAV-002 | POST /click | Link click | Yes | Yes |
| NAV-003 | POST /back | History back | Yes | Yes |
| NAV-004 | POST /forward | History forward | Yes | Yes |
| NAV-005 | POST /reload | Page reload | No | Yes |
| NAV-006 | POST /reload | Cache bypass | No | No |
| NAV-007 | POST /navigate | Referrer | No | No |
| NAV-008 | GET /tabs/{id} | Tab info | No | No |
| NAV-009 | POST /click | New tab popup | No | Yes |
| NAV-010 | POST /stop | Stop loading | No | No |
| SHOT-001 | POST /screenshot | No markup | Yes | No |
| SHOT-002 | POST /screenshot | Interactive | Yes | No |
| SHOT-003 | POST /screenshot | Clickable | Yes | No |
| SHOT-004 | POST /screenshot | Typeable | Yes | No |
| SHOT-005 | POST /screenshot | Inputs | Yes | No |
| SHOT-006 | POST /screenshot | Cursor visible | Yes | No |
| SHOT-007 | POST /screenshot | Cursor hidden | Yes | No |
| SHOT-008 | GET /screenshot | Binary response | Yes | No |
| SHOT-009 | POST /screenshot | Disabled exclude | Yes | No |
| SHOT-010 | POST /screenshot | Hidden exclude | Yes | No |
| DLG-001 | POST /click | Trigger alert | No | Yes |
| DLG-002 | GET /dialog | Get dialog info | No | No |
| DLG-003 | POST /dialog/accept | Accept alert | No | No |
| DLG-004 | Multiple | Accept confirm | No | Yes |
| DLG-005 | POST /dialog/dismiss | Dismiss confirm | No | No |
| DLG-006 | POST /dialog/accept | Prompt with text | No | No |
| DLG-007 | POST /dialog/dismiss | Dismiss prompt | No | No |
| DLG-008 | GET /dialog | No dialog | No | No |
| FILE-001 | POST /click | File chooser event | No | Yes |
| FILE-002 | POST /file-chooser | Provide file | No | Yes |
| FILE-003 | POST /file-chooser | Cancel chooser | No | Yes |
| FILE-004 | POST /file-chooser | Multiple files | No | No |
| FILE-005 | POST /file-chooser | Invalid ID | No | No |
| DL-001 | POST /click | Download started | No | Yes |
| DL-002 | GET /downloads | List downloads | No | No |
| DL-003 | GET /downloads/{id} | Download status | No | No |
| DL-004 | Multiple | Download complete | No | Yes |
| DL-005 | POST /downloads/cancel | Cancel download | No | No |
| SCROLL-001 | POST /scroll | Scroll down | Yes | Yes |
| SCROLL-002 | POST /scroll | Scroll up | No | Yes |
| SCROLL-003 | POST /click | Scroll in response | No | No |
| SCROLL-004 | Multiple | Click at scroll pos | No | No |
| SCROLL-005 | Multiple | Cursor after scroll | Yes | No |
| EXEC-001 | POST /execute | String result | No | No |
| EXEC-002 | POST /execute | Number result | No | No |
| EXEC-003 | POST /execute | Object result | No | No |
| EXEC-004 | POST /execute | Async/Promise | No | No |
| EXEC-005 | POST /execute | Timeout | No | No |
| EXEC-006 | POST /execute | Error handling | No | No |
| PAUSE-001 | GET /execution | Get state | No | No |
| PAUSE-002 | POST /execute | Frozen timer | No | No |
| PAUSE-003 | POST /execute | Frozen clock | No | No |
| PAUSE-004 | POST /execution | Resume | No | No |
| PAUSE-005 | POST /execution | Pause | No | No |
| PAUSE-006 | POST /screenshot | Virtual time | No | No |
| WAIT-001 | POST /click | action_complete | No | No |
| WAIT-002 | POST /click | immediate | No | No |
| WAIT-003 | POST /click | time wait | No | No |

---

## 5. Execution Order

Tests should be executed in the following order to manage dependencies and ensure proper state:

### Phase 1: Browser Initialization
1. Start ABP-enabled Chrome
2. Wait for `GET /browser/status` to return `ready: true`
3. Create test tab via `POST /tabs`

### Phase 2: Click and Input Tests (click-input-test.html)
Execute in order:
1. NAV-001 (navigate to test page)
2. CLICK-001 through CLICK-004
3. TYPE-001 through TYPE-003
4. KEY-001 through KEY-005
5. FORM-001

### Phase 3: Navigation Tests (navigation-test.html)
Execute in order:
1. NAV-001 (fresh navigation)
2. NAV-002 (link click)
3. NAV-003 (back)
4. NAV-004 (forward)
5. NAV-005, NAV-006 (reload variants)
6. NAV-007 (referrer)
7. NAV-008 (tab info)
8. NAV-009 (new tab) - creates new tab, close after
9. NAV-010 (stop loading)

### Phase 4: Screenshot Tests (screenshot-markup-test.html)
Execute in any order (stateless):
1. Navigate to test page
2. SHOT-001 through SHOT-010

### Phase 5: Dialog and File Tests (dialog-file-test.html)
Execute in order:
1. Navigate to test page
2. DLG-001 through DLG-008
3. FILE-001 through FILE-005
4. DL-001 through DL-005

### Phase 6: Scroll and Execution Tests (scroll-execution-test.html)
Execute in order:
1. Navigate to test page
2. SCROLL-001 through SCROLL-005
3. EXEC-001 through EXEC-006
4. PAUSE-001 through PAUSE-006
5. WAIT-001 through WAIT-003

### Phase 7: Cleanup
1. Close test tabs
2. Shutdown browser via `POST /browser/shutdown`

---

## 6. Success Criteria

### 6.1 Individual Test Pass Criteria

A test passes if:
1. HTTP response status matches expected (200 for success, 4xx/5xx for expected errors)
2. All specified response fields are present and match expected values
3. Screenshot verification (when required) shows expected visual state
4. JavaScript execution verification (when required) returns expected values
5. Events (when expected) are captured with correct type and data

### 6.2 Overall Test Suite Pass Criteria

The test suite passes if:
- **All critical tests pass** (100% of: CLICK-001, TYPE-001, NAV-001, NAV-003, NAV-004, SHOT-001, SHOT-002, DLG-001, DLG-003, EXEC-001, PAUSE-002)
- **At least 95% of all tests pass**
- **No tests produce unexpected crashes or hangs**
- **No memory leaks detected** (browser memory stable across test run)

### 6.3 Test Failure Categories

| Category | Severity | Action |
|----------|----------|--------|
| Critical test failure | Blocker | Must fix before release |
| Non-critical test failure | Major | Should fix before release |
| Screenshot mismatch | Minor | Review manually, may be acceptable variance |
| Timing-dependent failure | Warning | Retry, adjust timeouts if persistent |
| Flaky test (passes on retry) | Info | Investigate root cause, improve test |

### 6.4 Test Report Requirements

Each test run should produce:
1. **Summary**: Total tests, passed, failed, skipped
2. **Timing**: Total duration, average test duration
3. **Failures**: Test ID, expected vs actual, screenshot diff (if applicable)
4. **Screenshots**: All captured screenshots archived for review
5. **Logs**: ABP server logs, browser console logs

---

## 7. Appendix: Element Coordinate Reference

For deterministic testing, all test pages use fixed-position elements. This table provides the expected click coordinates:

### click-input-test.html
| Element ID | X | Y | Type |
|------------|---|---|------|
| btn-fixed | 100 | 100 | Button |
| right-click-target | 300 | 100 | Div |
| double-click-target | 500 | 100 | Div |
| text-input | 100 | 200 | Input |
| text-area | 100 | 300 | Textarea |
| password-input | 100 | 400 | Input |
| submit-btn | 100 | 550 | Button |

### navigation-test.html
| Element ID | X | Y | Type |
|------------|---|---|------|
| link-page-2 | 100 | 150 | Link |
| link-page-3 | 100 | 200 | Link |
| link-external | 100 | 250 | Link |
| link-new-tab | 100 | 300 | Link |

### screenshot-markup-test.html
| Row | Y | Element Types |
|-----|---|---------------|
| 1 | 100 | Buttons, Links (clickable) |
| 2 | 200 | Text inputs, Textarea (typeable) |
| 3 | 300 | Checkbox, Radio, Select (inputs) |
| 4 | 400 | Static, Disabled (non-interactive) |
| 5 | 500 | Edge cases |

### dialog-file-test.html
| Element ID | X | Y | Type |
|------------|---|---|------|
| btn-alert | 100 | 100 | Button |
| btn-confirm | 250 | 100 | Button |
| btn-prompt | 400 | 100 | Button |
| file-single | 100 | 300 | File input |
| file-multiple | 300 | 300 | File input |
| download-link | 100 | 500 | Link |

### scroll-execution-test.html
| Element ID | X | Y (page) | Notes |
|------------|---|----------|-------|
| btn-top | 100 | 100 | Visible at scroll 0 |
| btn-middle | 100 | 2500 | Visible at scroll ~2000 |
| btn-bottom | 100 | 4900 | Visible at scroll ~4200 |
| cursor-area | 300 | 1500 | 500x500px test area |
