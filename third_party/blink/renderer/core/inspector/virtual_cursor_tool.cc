// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/inspector/virtual_cursor_tool.h"

#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/inspector/inspector_cursor_drawer.h"
#include "third_party/blink/renderer/core/inspector/inspector_overlay_agent.h"
#include "third_party/blink/renderer/core/layout/hit_test_location.h"
#include "third_party/blink/renderer/core/layout/hit_test_request.h"
#include "third_party/blink/renderer/core/layout/hit_test_result.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/platform/graphics/graphics_context.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_canvas.h"
#include "ui/base/cursor/cursor.h"

namespace blink {

namespace {

// Overlay name for the virtual cursor tool
const char kVirtualCursorOverlayName[] = "virtualCursor";

}  // namespace

VirtualCursorTool::VirtualCursorTool(InspectorOverlayAgent* overlay,
                                     OverlayFrontend* frontend)
    : InspectTool(overlay, frontend) {}

void VirtualCursorTool::SetPosition(float x, float y) {
  position_ = gfx::PointF(x, y);
}

void VirtualCursorTool::SetVisible(bool visible) {
  visible_ = visible;
}

void VirtualCursorTool::DetectCursorStyle() {
  LocalFrame* frame = overlay_->GetFrame();
  if (!frame || !frame->View() || !frame->ContentLayoutObject()) {
    cursor_type_ = ui::mojom::blink::CursorType::kPointer;
    is_custom_cursor_ = false;
    return;
  }

  // Convert CSS pixel position to physical pixels for hit-testing
  gfx::PointF point_in_frame = position_;

  // Create hit test request with appropriate flags
  HitTestRequest::HitTestRequestType hit_type =
      HitTestRequest::kReadOnly | HitTestRequest::kActive |
      HitTestRequest::kAllowChildFrameContent;
  HitTestRequest request(hit_type);

  // Perform hit-test at the cursor position
  HitTestLocation location(PhysicalOffset::FromPointFRound(point_in_frame));
  HitTestResult result(request, location);

  if (frame->ContentLayoutObject()) {
    frame->ContentLayoutObject()->HitTest(location, result);
  }

  // Use EventHandler::CursorForHitTest to determine the cursor type
  // This respects CSS cursor property, auto-detection for text/links, etc.
  std::optional<ui::Cursor> cursor =
      frame->GetEventHandler().CursorForHitTest(location, result);

  if (cursor.has_value()) {
    cursor_type_ = cursor->type();
    // Check if it's a custom cursor by looking at the cursor info
    is_custom_cursor_ = (cursor_type_ == ui::mojom::blink::CursorType::kCustom);
  } else {
    // Default to pointer if no cursor could be determined
    cursor_type_ = ui::mojom::blink::CursorType::kPointer;
    is_custom_cursor_ = false;
  }
}

String VirtualCursorTool::GetOverlayName() {
  return kVirtualCursorOverlayName;
}

void VirtualCursorTool::Draw(float scale) {
  // Virtual cursor drawing is handled directly in PaintOverlayPage()
  // using InspectorCursorDrawer::DrawCursor() on the native canvas.
  // This method is intentionally empty as the cursor is drawn via
  // the overlay agent's native drawing path.
}

bool VirtualCursorTool::ForwardEventsToOverlay() {
  // Don't forward events - this is a display-only tool
  return false;
}

bool VirtualCursorTool::HideOnHideHighlight() {
  // Don't hide when hideHighlight is called
  return false;
}

bool VirtualCursorTool::HideOnMouseMove() {
  // Don't hide on mouse move
  return false;
}

}  // namespace blink
