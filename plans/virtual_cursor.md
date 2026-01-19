# Virtual Cursor Implementation Plan

## Input-Driven State + Compositor Layer

### Overview

Implement a virtual cursor that combines centralized state management in the browser process with native compositor-layer rendering. The cursor position is automatically updated by input actions (click, scroll, move), eliminating the need for separate cursor API calls. Rendering occurs at the CC (Chromium Compositor) layer, completely independent of Blink's rendering pipeline and JavaScript.

### Design Principles

1. **Input-driven positioning** - Cursor moves automatically with input actions (click, scroll, etc.)
2. **Input pipeline integration** - RenderInputRouter intercepts mouse events and forwards position updates
3. **Controlled source tracking** - CDP events always captured; system events only with `--allow-system-inputs`
4. **Always present** - Cursor is visible in viewport whenever ABP is enabled
5. **No JavaScript dependency** - Cursor rendering at compositor level using Skia
6. **Screenshot transparency** - Screenshot endpoint captures what's rendered, doesn't paint cursors
7. **Screenshot configurability** - Cursor visibility controllable via layer visibility toggle
8. **Single source of truth** - Cursor state flows from RenderInputRouter → AbpController → Renderer

---

## Architecture

### Layer Structure

```
┌─────────────────────────────────────────────┐
│           Final Composited Frame            │
├─────────────────────────────────────────────┤
│  ┌───────────────────────────────────────┐  │
│  │     Virtual Cursor Layer (TOP)        │  │  ← Dedicated compositor layer
│  │     - PictureLayer with cursor bitmap │  │
│  │     - Transform-based positioning     │  │
│  │     - SetHideLayerAndSubtree() for    │  │
│  │       screenshot visibility control   │  │
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  ┌───────────────────────────────────────┐  │
│  │     Inspector Overlay Layer           │  │  ← Existing (markup overlays)
│  └───────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  ┌───────────────────────────────────────┐  │
│  │     Content Layers                    │  │  ← Page content
│  │     - Document root layer             │  │
│  │     - Composited elements             │  │
│  └───────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
```

### Component Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Browser Process                                │
│                                                                          │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │  AbpController                                                      │ │
│  │  ┌──────────────────────┐                                          │ │
│  │  │ VirtualCursorState   │  ← Authoritative cursor state            │ │
│  │  │ - position (x, y)    │                                          │ │
│  │  │ - cursor_type        │                                          │ │
│  │  │ - visible            │                                          │ │
│  │  └──────────────────────┘                                          │ │
│  │                                                                     │ │
│  │  OnVirtualCursorMoved(x, y) ← Called by RenderInputRouter (ALL events) │
│  │  - Updates VirtualCursorState                                       │ │
│  │  - Forwards to renderer via Mojo                                    │ │
│  └─────────────┬──────────────────────────────────────────────────────┘ │
│                │                                                         │
│                ▼                                                         │
│  ┌─────────────────────────────────────────────────────────────────────┐│
│  │  RenderWidgetHostImpl                                               ││
│  │  - Receives cursor state updates from AbpController                 ││
│  │  - Sends to renderer via Mojo IPC                                   ││
│  └─────────────┬───────────────────────────────────────────────────────┘│
└────────────────┼────────────────────────────────────────────────────────┘
                 │ Mojo IPC
                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                          Renderer Process                               │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐ │
│  │  RenderInputRouter (INPUT INTERCEPTION POINT)                     │ │
│  │  - Intercepts mouse events in input pipeline                      │ │
│  │  - Captures position: CDP always, system only w/ --allow-system   │ │
│  │  - Notifies AbpController of position changes                      │ │
│  │  - Blocks real system input when ABP enabled                       │ │
│  └───────────────────────────────────────────────────────────────────┘ │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐ │
│  │  VirtualCursorLayerManager                                        │ │
│  │  - Creates/manages VirtualCursorLayer                             │ │
│  │  - Handles Mojo messages from browser                             │ │
│  │  - Updates layer transform for position changes                   │ │
│  └───────────────────────────────────────────────────────────────────┘ │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐ │
│  │  VirtualCursorLayer (cc::PictureLayer)                            │ │
│  │  - Dedicated compositor layer                                      │ │
│  │  - Transform-based positioning (no repaints)                       │ │
│  │  - Cached cursor bitmaps per type                                  │ │
│  │  - SetHideLayerAndSubtree() for visibility control                │ │
│  └───────────────────────────────────────────────────────────────────┘ │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐ │
│  │  Blink (hit-testing only, async)                                  │ │
│  │  - VirtualCursorTool::DetectCursorStyle()                         │ │
│  │  - Returns cursor type for element at position                     │ │
│  │  - Triggered via CDP, results sent back to AbpController          │ │
│  └───────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Data Flow

### Input Action Flow (e.g., Click)

```
1. REST API Request
   POST /api/v1/tabs/{id}/click {x: 100, y: 200}

2. AbpController (Browser - UI Thread)
   ┌─────────────────────────────────────────────────────┐
   │ // Dispatch click via CDP (Input.dispatchMouseEvent)│
   │ // Event has kFromDebugger flag set                 │
   │ SendCDP("Input.dispatchMouseEvent", {               │
   │     type: "mousePressed", x: 100, y: 200            │
   │ });                                                 │
   └─────────────────────────────────────────────────────┘

3. RenderInputRouter (Renderer Process - INPUT INTERCEPTION)
   ┌─────────────────────────────────────────────────────┐
   │ // Capture position from mouse events               │
   │ if (abp_enabled_ &&                                 │
   │     WebInputEvent::IsMouseEventType(event.GetType())){│
   │                                                     │
   │   bool is_cdp = event.GetModifiers() & kFromDebugger;│
   │   bool allow_system = allow_system_inputs_;         │
   │                                                     │
   │   // Only capture: CDP events OR system when allowed│
   │   if (is_cdp || allow_system) {                     │
   │     float x = event.PositionInWidget().x();         │
   │     float y = event.PositionInWidget().y();         │
   │     NotifyVirtualCursorMoved(x, y);                 │
   │   }                                                 │
   │ }                                                   │
   │ // Continue normal event processing...              │
   └─────────────────────────────────────────────────────┘

4. AbpController::OnVirtualCursorMoved (Browser - UI Thread)
   ┌─────────────────────────────────────────────────────┐
   │ // Update authoritative cursor state                │
   │ cursor_state_.position = {100, 200};                │
   │ cursor_state_.visible = true;                       │
   │                                                     │
   │ // Send position to compositor layer (Mojo)         │
   │ render_widget_host_->SetVirtualCursorPosition(      │
   │     100, 200, true);                                │
   │                                                     │
   │ // Request cursor style detection (async CDP)       │
   │ SendCDP("Overlay.setVirtualCursor", {x, y});        │
   └─────────────────────────────────────────────────────┘

5. Parallel Paths:

   A) Compositor Path (immediate)
      RenderWidgetHostImpl → Mojo → VirtualCursorLayerManager
      → cursor_layer_->SetTransform(translate(100, 200))
      → LayerTreeHost::SetNeedsCommit()
      → Frame composited with cursor at new position

   B) CDP Path (async, for cursor style)
      DevToolsAgentHost → InspectorOverlayAgent
      → VirtualCursorTool::DetectCursorStyle()
      → Returns "hand" (e.g., hovering over link)
      → AbpController updates cursor_state_.cursor_type
      → Mojo → VirtualCursorLayerManager::SetCursorType()
      → Layer repaints with new cursor bitmap
```

### Screenshot Flow (Default - With Cursor)

```
1. Screenshot Request
   POST /api/v1/tabs/{id}/screenshot
   // or explicitly: {cursor: true}

2. AbpController
   ┌─────────────────────────────────────────────────────┐
   │ // Cursor layer already visible at last position   │
   │ // No cursor manipulation needed                   │
   │                                                     │
   │ // Capture the composited frame                     │
   │ rwhv->CopyFromSurface(...);                        │
   └─────────────────────────────────────────────────────┘

3. Compositor
   - Composites all layers including cursor layer
   - Cursor automatically included in output

4. Result
   - Screenshot contains cursor at its current position
```

### Screenshot Flow (Without Cursor)

```
1. Screenshot Request
   POST /api/v1/tabs/{id}/screenshot {cursor: false}

2. AbpController
   ┌─────────────────────────────────────────────────────┐
   │ // Hide cursor layer                                │
   │ render_widget_host_->SetVirtualCursorVisible(false);│
   │                                                     │
   │ // Wait for compositor commit                       │
   │ WaitForCompositorCommit();                          │
   │                                                     │
   │ // Capture the composited frame                     │
   │ rwhv->CopyFromSurface(..., callback);              │
   └─────────────────────────────────────────────────────┘

3. Callback: OnScreenshotCaptured
   ┌─────────────────────────────────────────────────────┐
   │ // Restore cursor visibility                        │
   │ render_widget_host_->SetVirtualCursorVisible(true); │
   │                                                     │
   │ // Return screenshot data                           │
   │ SendResponse(screenshot_data);                      │
   └─────────────────────────────────────────────────────┘

4. Result
   - Screenshot captured without cursor
   - Cursor visibility restored for normal operation
```

### Autocapture Flow (Before/After Screenshots)

```
1. Action with Autocapture
   POST /api/v1/tabs/{id}/click {x: 100, y: 200}
   // Autocapture enabled in session settings

2. Before Screenshot
   ┌─────────────────────────────────────────────────────┐
   │ // Cursor at previous position (e.g., 50, 50)       │
   │ CopyFromSurface() → before_screenshot               │
   │ // Includes cursor at (50, 50)                      │
   └─────────────────────────────────────────────────────┘

3. Update Cursor & Execute Action
   ┌─────────────────────────────────────────────────────┐
   │ // Move cursor to new position                      │
   │ cursor_state_.position = {100, 200};                │
   │ SendToRenderer: SetVirtualCursorPosition(100, 200); │
   │                                                     │
   │ // Dispatch click                                   │
   │ DispatchMouseEvent(click, 100, 200);                │
   └─────────────────────────────────────────────────────┘

4. After Screenshot
   ┌─────────────────────────────────────────────────────┐
   │ // Cursor now at new position (100, 200)            │
   │ CopyFromSurface() → after_screenshot                │
   │ // Includes cursor at (100, 200)                    │
   └─────────────────────────────────────────────────────┘
```

---

## Component Responsibilities

### 1. RenderInputRouter (Renderer Process) - INPUT INTERCEPTION

**Location:** `components/input/render_input_router.cc`

**Responsibilities:**
- Intercept mouse events flowing through the input pipeline
- Extract cursor position from CDP events (always) and system events (only if `--allow-system-inputs`)
- Notify browser process of virtual cursor position changes via Mojo IPC
- Block real system input when ABP is enabled (existing behavior, unchanged)

**Key Logic:**
```cpp
blink::mojom::InputEventResultState RenderInputRouter::FilterInputEvent(
    const WebInputEvent& event) {

  // NEW: Capture cursor position from mouse events
  if (abp_enabled_ && WebInputEvent::IsMouseEventType(event.GetType())) {
    const auto& mouse_event = static_cast<const WebMouseEvent&>(event);

    bool is_cdp_event = event.GetModifiers() & WebInputEvent::kFromDebugger;

    // Only capture: CDP events OR system events when --allow-system-inputs
    if (is_cdp_event || allow_system_inputs_) {
      float x = mouse_event.PositionInWidget().x();
      float y = mouse_event.PositionInWidget().y();
      virtual_cursor_client_->OnVirtualCursorMoved(x, y);
    }
  }

  // Existing: Block real system input when ABP enabled
  if (abp_enabled_ && !allow_system_inputs_ &&
      !(event.GetModifiers() & WebInputEvent::kFromDebugger)) {
    return kNoConsumerExists;  // Block real input
  }

  // Continue normal processing...
}
```

**Position capture rules:**
- CDP events (`kFromDebugger` flag) - always captured
- System events - only captured if `--allow-system-inputs` is passed

**Mojo Interface (Renderer → Browser):**
```cpp
// New interface for cursor position updates
interface VirtualCursorClient {
  OnVirtualCursorMoved(float x, float y);
};
```

### 2. AbpController (Browser Process)

**Location:** `chrome/browser/abp/abp_controller.cc`

**Data Structure:**
```cpp
struct VirtualCursorState {
  gfx::PointF position;                    // Current position (viewport coords)
  ui::mojom::CursorType cursor_type;       // Detected cursor style
  bool visible = true;                      // Visibility state
  bool enabled = false;                     // ABP cursor feature enabled
};

// Per-tab cursor state
std::map<std::string, VirtualCursorState> cursor_states_;
```

**Responsibilities:**
- Maintain authoritative cursor state per tab
- Receive cursor position updates from RenderInputRouter via `OnVirtualCursorMoved()`
- Send cursor updates to renderer compositor layer via RenderWidgetHost
- Coordinate with Blink for async cursor style detection
- Handle screenshot cursor visibility toggle

**Key Methods:**
```cpp
// Called by RenderInputRouter when CDP mouse event is intercepted
void OnVirtualCursorMoved(const std::string& tab_id, float x, float y);

void UpdateCursorType(const std::string& tab_id, ui::mojom::CursorType type);
void SetCursorVisible(const std::string& tab_id, bool visible);
void DetectCursorStyleAsync(const std::string& tab_id, float x, float y);
```

### 3. RenderWidgetHostImpl (Browser Process)

**Location:** `content/browser/renderer_host/render_widget_host_impl.cc`

**Responsibilities:**
- Receive cursor state updates from AbpController
- Forward to renderer process via Mojo IPC
- No cursor state storage (AbpController is source of truth)

**New Methods:**
```cpp
void SetVirtualCursorPosition(float x, float y, bool visible);
void SetVirtualCursorType(ui::mojom::CursorType type);
void SetVirtualCursorVisible(bool visible);
```

### 3. VirtualCursorLayerManager (Renderer Process)

**Location:** `content/renderer/virtual_cursor_layer_manager.h/cc`

**Responsibilities:**
- Create and manage VirtualCursorLayer lifecycle
- Handle Mojo messages from browser process
- Update layer transform for position changes (no repaint)
- Update layer content for cursor type changes (repaint)
- Ensure layer is always on top of content layers

**Interface:**
```cpp
class VirtualCursorLayerManager : public mojom::VirtualCursor {
 public:
  void Initialize(cc::LayerTreeHost* layer_tree_host);
  void Shutdown();

  // mojom::VirtualCursor implementation
  void SetPosition(float x, float y, bool visible) override;
  void SetCursorType(ui::mojom::CursorType type) override;
  void SetVisible(bool visible) override;

 private:
  scoped_refptr<VirtualCursorLayer> cursor_layer_;
  cc::LayerTreeHost* layer_tree_host_;
};
```

### 4. VirtualCursorLayer (Renderer Process)

**Location:** `content/renderer/virtual_cursor_layer.h/cc`

**Responsibilities:**
- Implement cc::ContentLayerClient interface
- Generate cursor bitmap based on cursor type using Skia
- Cache cursor bitmaps for each type to avoid regeneration
- Use transform for position updates (efficient, no repaint)
- Support visibility toggle via SetHideLayerAndSubtree()

**Key Features:**
```cpp
class VirtualCursorLayer : public cc::PictureLayer,
                           public cc::ContentLayerClient {
 public:
  void SetPosition(const gfx::PointF& position);
  void SetCursorType(ui::mojom::CursorType type);
  void SetVisible(bool visible);

  // cc::ContentLayerClient
  gfx::Rect PaintableRegion() const override;
  scoped_refptr<cc::DisplayItemList> PaintContentsToDisplayList() override;
  bool FillsBoundsCompletely() const override { return false; }

 private:
  gfx::PointF position_;
  ui::mojom::CursorType cursor_type_ = ui::mojom::CursorType::kPointer;

  // Cached bitmaps per cursor type
  std::map<ui::mojom::CursorType, sk_sp<SkImage>> cursor_cache_;

  void DrawCursor(cc::PaintCanvas* canvas);
  sk_sp<SkImage> GetOrCreateCursorBitmap(ui::mojom::CursorType type);
};
```

### 5. VirtualCursorTool (Blink - Existing, Modified)

**Location:** `third_party/blink/renderer/core/inspector/virtual_cursor_tool.cc`

**Responsibilities (unchanged):**
- Perform hit-testing at cursor position
- Detect CSS cursor style from elements
- Report detected cursor type back via CDP response

**Note:** This component is only used for cursor style detection, not rendering.

---

## Mojo Interface Definition

### Browser → Renderer (Cursor Layer Control)

**Location:** `content/common/virtual_cursor.mojom`

```mojom
module content.mojom;

import "ui/base/cursor/mojom/cursor_type.mojom";

// Browser → Renderer interface for virtual cursor control
interface VirtualCursor {
  // Update cursor position and visibility
  SetPosition(float x, float y, bool visible);

  // Update cursor type/style (after async detection)
  SetCursorType(ui.mojom.CursorType cursor_type);

  // Toggle visibility only (for screenshots)
  SetVisible(bool visible);

  // Enable/disable virtual cursor feature
  SetEnabled(bool enabled);
};
```

### Renderer → Browser (Input Pipeline → AbpController)

**Location:** `content/common/virtual_cursor_client.mojom`

```mojom
module content.mojom;

// Renderer → Browser interface for cursor position updates
// Used by RenderInputRouter to notify AbpController of CDP mouse events
interface VirtualCursorClient {
  // Called when RenderInputRouter intercepts a CDP mouse event
  // Provides the cursor position from the WebMouseEvent
  OnVirtualCursorMoved(float x, float y);

  // Notify browser when cursor layer is ready
  OnCursorLayerReady();

  // Notify browser when visibility change is committed
  OnVisibilityChanged(bool visible);
};
```

### Data Flow Summary

```
┌─────────────────┐     CDP Input.dispatch*      ┌─────────────────────┐
│  AbpController  │ ─────────────────────────────►│  RenderInputRouter  │
└─────────────────┘                               └──────────┬──────────┘
        ▲                                                    │
        │  VirtualCursorClient::OnVirtualCursorMoved(x,y)   │
        └────────────────────────────────────────────────────┘
        │
        │  VirtualCursor::SetPosition(x, y, visible)
        ▼
┌─────────────────────────┐
│  VirtualCursorLayer     │
│  (compositor layer)     │
└─────────────────────────┘
```

---

## Implementation Phases

### Phase 1: Mojo Interface & Browser-Side State

**Goal:** Establish cursor state management in AbpController and IPC channel

**Tasks:**
1. Define VirtualCursor Mojo interface
2. Add VirtualCursorState struct to AbpController
3. Implement cursor state update methods in AbpController
4. Wire Mojo interface through RenderWidgetHostImpl
5. Update input dispatch methods to call UpdateCursorPosition()

**Files to create:**
- `content/common/virtual_cursor.mojom`

**Files to modify:**
- `chrome/browser/abp/abp_controller.h/cc`
- `content/browser/renderer_host/render_widget_host_impl.h/cc`
- `content/public/browser/render_widget_host.h`

### Phase 2: Compositor Layer Infrastructure

**Goal:** Create the VirtualCursorLayer in the renderer

**Tasks:**
1. Create VirtualCursorLayer class extending cc::PictureLayer
2. Create VirtualCursorLayerManager to handle Mojo messages
3. Implement layer creation and z-ordering
4. Add transform-based positioning (no content repaint)
5. Integrate with RenderWidget initialization

**Files to create:**
- `content/renderer/virtual_cursor_layer.h/cc`
- `content/renderer/virtual_cursor_layer_manager.h/cc`

**Files to modify:**
- `content/renderer/render_widget.h/cc`
- `content/renderer/BUILD.gn`

### Phase 3: Cursor Bitmap Generation

**Goal:** Generate and cache cursor images for all types

**Tasks:**
1. Port cursor drawing code from InspectorCursorDrawer
2. Implement bitmap caching per cursor type
3. Handle device scale factor for crisp rendering
4. Support all standard cursor types

**Cursor Types to Support:**
- Pointer (default arrow)
- Hand (clickable)
- Text (I-beam)
- Crosshair
- Move
- Resize variants (n, s, e, w, ne, nw, se, sw, ew, ns, nesw, nwse)
- Wait/Progress
- Not-allowed / No-drop
- Grab/Grabbing
- Help
- Context-menu
- Cell
- Vertical-text
- Alias
- Copy
- Zoom-in / Zoom-out

### Phase 4: Input Pipeline Integration

**Goal:** Intercept mouse events in RenderInputRouter and forward position updates

**Tasks:**
1. Add Mojo interface `VirtualCursorClient` for renderer → browser communication
2. Modify RenderInputRouter to intercept mouse events
3. Add `abp_enabled_` and `allow_system_inputs_` flags to RenderInputRouter
4. Capture position from CDP events (always) and system events (only if `--allow-system-inputs`)
5. Extract position from WebMouseEvent and call `OnVirtualCursorMoved()`
6. Implement `AbpController::OnVirtualCursorMoved()` to update state and forward to compositor
7. Wire Mojo binding between RenderInputRouter and AbpController
8. Handle all mouse event types: mousePressed, mouseMoved, mouseReleased, wheel

**Files to create:**
- `content/common/virtual_cursor_client.mojom` (renderer → browser interface)

**Files to modify:**
- `components/input/render_input_router.cc` (intercept mouse events, check flags)
- `components/input/render_input_router.h` (add VirtualCursorClient remote, abp_enabled_, allow_system_inputs_ flags)
- `chrome/browser/abp/abp_controller.cc` (implement OnVirtualCursorMoved)
- `chrome/browser/abp/abp_controller.h` (add VirtualCursorClient receiver)

### Phase 5: Screenshot Integration

**Goal:** Clean screenshot implementation with cursor visibility control

**Tasks:**
1. Remove cursor painting from screenshot endpoint
2. Add `cursor` parameter to screenshot options (default: true)
3. Implement visibility toggle before/after capture
4. Add synchronization to wait for compositor commit
5. Test with CopyFromSurface and CDP Page.captureScreenshot

**Files to modify:**
- `chrome/browser/abp/abp_controller.cc` (screenshot methods)

### Phase 6: Cursor Style Detection

**Goal:** Async cursor style detection via existing CDP path

**Tasks:**
1. Keep existing VirtualCursorTool for hit-testing
2. Trigger detection after position update (async)
3. Update cursor type in compositor layer when detection completes
4. Handle detection failures gracefully (keep current type)

**Files to modify:**
- `chrome/browser/abp/abp_controller.cc` (CDP integration)

### Phase 7: Cleanup & Optimization

**Goal:** Remove old implementation, optimize performance

**Tasks:**
1. Remove JavaScript cursor injection code
2. Remove cursor painting from screenshot capture
3. Remove 200ms delay in screenshot flow
4. **Remove AbpMouseTracker** (tracks real system mouse, not needed with input pipeline integration)
5. Profile and optimize layer update performance
6. Verify no Blink repaints on cursor position changes

**Files to delete:**
- `chrome/browser/abp/abp_mouse_tracker.h`
- `chrome/browser/abp/abp_mouse_tracker.cc`
- `chrome/browser/abp/abp_mouse_tracker_mac.mm`

**Files to modify:**
- `chrome/browser/abp/abp_controller.cc` (remove AbpMouseTracker usage)
- `chrome/browser/abp/BUILD.gn` (remove AbpMouseTracker from build)
- `third_party/blink/renderer/core/inspector/inspector_overlay_agent.cc`

---

## API Changes

### Screenshot Endpoint

**Before:**
```
POST /api/v1/tabs/{id}/screenshot
{
  "format": "webp",
  "quality": 80,
  "markup": "interactive",
  "mouse": "normal"        // ← Controlled cursor painting
}
```

**After:**
```
POST /api/v1/tabs/{id}/screenshot
{
  "format": "webp",
  "quality": 80,
  "markup": "interactive",
  "cursor": true           // ← Controls layer visibility (default: true)
}
```

---

## Key Considerations

### Z-Ordering

The cursor layer must always render on top of:
- Page content layers
- Fixed/sticky positioned elements
- CSS transforms with high z-index
- Inspector overlay markup (if both are visible)

**Strategy:** Add cursor layer as the last child of the root layer after all other layers are added.

```cpp
void VirtualCursorLayerManager::Initialize(cc::LayerTreeHost* host) {
  cursor_layer_ = VirtualCursorLayer::Create();

  // Add as last child to ensure topmost z-order
  host->root_layer()->AddChild(cursor_layer_);

  // Or use a dedicated overlay layer tree if available
}
```

### Coordinate Systems

- **API coordinates** - Viewport-relative CSS pixels (what AbpController receives)
- **Layer coordinates** - Compositor layer space
- **Device pixels** - Physical screen pixels (for bitmap rendering)

**Strategy:**
```cpp
void VirtualCursorLayer::SetPosition(const gfx::PointF& viewport_pos) {
  // Convert viewport coordinates to layer transform
  gfx::Transform transform;
  transform.Translate(viewport_pos.x(), viewport_pos.y());
  SetTransform(transform);

  // Note: Bitmap rendered at device scale factor for crispness
}
```

### Performance

**Position updates (hot path):**
- Transform-only update, no content repaint
- Single Mojo message per position change
- No CDP round-trip for position

**Cursor type updates (less frequent):**
- Requires layer content repaint
- Bitmap cached, only repaint if type changes
- Async detection doesn't block input dispatch

**Metrics to track:**
- Frame time impact of cursor layer
- IPC latency for cursor updates
- Memory usage of cursor bitmap cache

### Thread Safety

| Component | Thread |
|-----------|--------|
| AbpController | Browser UI thread |
| RenderWidgetHostImpl | Browser UI thread |
| VirtualCursorLayerManager | Renderer main thread |
| VirtualCursorLayer | Compositor thread (via proxy) |

**Strategy:** Use Mojo for cross-process communication, PostTask for cross-thread within process.

### Edge Cases

1. **Tab switching** - Cursor state per-tab, only active tab's cursor rendered
2. **Navigation** - Cursor layer survives same-origin navigation, reset on cross-origin
3. **Renderer crash** - Cursor state in browser survives, restored on new renderer
4. **Multiple windows** - Each window/tab has independent cursor state
5. **Offscreen tabs** - Cursor state tracked but layer not rendered
6. **Page with `cursor: none`** - Still render virtual cursor (we're simulating, not following page CSS)
7. **iframes** - Cursor detection works across iframe boundaries via hit-testing

---

## Testing Strategy

### Unit Tests

1. VirtualCursorState management in AbpController
2. Cursor bitmap generation for all cursor types
3. Coordinate transformation accuracy
4. Layer z-ordering verification
5. Mojo message serialization/deserialization

### Integration Tests

1. Input action updates cursor position (click, scroll, move)
2. Screenshot includes cursor by default
3. Screenshot excludes cursor when `cursor: false`
4. Cursor style detection returns correct type for elements
5. Rapid cursor movement stability (no frame drops)
6. Navigation preserves/resets cursor state appropriately
7. Autocapture before/after shows cursor at correct positions

### Performance Tests

1. Cursor update latency (input dispatch to screen update)
2. Frame rate impact with cursor layer active
3. Memory usage with all cursor types cached
4. Comparison with previous JavaScript/overlay approach

### Manual Testing

1. Visual verification of cursor appearance on various pages
2. Cursor movement smoothness during rapid input
3. Cursor style changes when hovering different elements
4. Screenshot accuracy with and without cursor
5. Behavior during page load, navigation, scroll

---

## Migration Plan

### Phase 1: Parallel Implementation
- Implement new compositor cursor behind flag `--abp-compositor-cursor`
- Keep existing inspector overlay cursor as default
- Test both implementations in parallel

### Phase 2: Gradual Rollout
- Enable compositor cursor by default with ABP
- Keep overlay cursor available via `--abp-legacy-cursor`
- Monitor for issues

### Phase 3: Cleanup
- Remove inspector overlay cursor code
- Remove cursor-related code from screenshot endpoint
- Remove legacy flag
- Update documentation

---

## Open Questions (Resolved)

1. **Should cursor layer be part of the main layer tree or a separate overlay tree?**
   → Main layer tree, as last child of root for correct z-ordering.

2. **How to handle cursor during compositor-driven animations?**
   → Cursor layer updates independently of content animations.

3. **Should we support animated cursors (e.g., loading spinner)?**
   → Deferred. Start with static cursors, can add animation support later.

4. **How to handle cursor when page has `cursor: none` CSS?**
   → Render virtual cursor regardless. We're simulating user input, not following page styling.

5. **Should cursor be visible during scroll/pinch gestures?**
   → Yes, cursor updates to scroll origin position.

---

## References

- `cc/layers/picture_layer.cc` - Base layer implementation
- `cc/layers/ui_resource_layer.cc` - Alternative using pre-rendered textures
- `content/renderer/render_widget.cc` - Main integration point
- `ui/base/cursor/` - System cursor definitions
- `third_party/blink/renderer/core/inspector/inspector_cursor_drawer.cc` - Existing Skia cursor drawing
- `third_party/blink/renderer/core/inspector/virtual_cursor_tool.cc` - Existing hit-testing
