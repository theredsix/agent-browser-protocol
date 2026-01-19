// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_VIRTUAL_CURSOR_TOOL_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_VIRTUAL_CURSOR_TOOL_H_

#include "third_party/blink/renderer/core/inspector/inspector_overlay_agent.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-blink.h"
#include "ui/gfx/geometry/point_f.h"

namespace blink {

// VirtualCursorTool renders a virtual cursor overlay at a specified position.
// It performs hit-testing to detect the appropriate cursor style based on
// CSS cursor properties and element types at the cursor position.
class CORE_EXPORT VirtualCursorTool final : public InspectTool {
 public:
  VirtualCursorTool(InspectorOverlayAgent* overlay, OverlayFrontend* frontend);
  VirtualCursorTool(const VirtualCursorTool&) = delete;
  VirtualCursorTool& operator=(const VirtualCursorTool&) = delete;
  ~VirtualCursorTool() override = default;

  // Update cursor position in CSS pixels (viewport-relative)
  void SetPosition(float x, float y);

  // Set cursor visibility
  void SetVisible(bool visible);

  // Get the detected cursor type after calling DetectCursorStyle()
  ui::mojom::blink::CursorType GetDetectedCursorType() const {
    return cursor_type_;
  }

  // Returns true if the detected cursor is a custom cursor image
  bool IsCustomCursor() const { return is_custom_cursor_; }

  // Get cursor position in CSS pixels
  const gfx::PointF& GetPosition() const { return position_; }

  // Check if cursor is visible
  bool IsVisible() const { return visible_; }

  // Perform hit-testing at current position to detect cursor style
  void DetectCursorStyle();

  // InspectTool overrides
  String GetOverlayName() override;
  void Draw(float scale) override;
  bool ForwardEventsToOverlay() override;
  bool HideOnHideHighlight() override;
  bool HideOnMouseMove() override;

 private:
  gfx::PointF position_;
  bool visible_ = true;
  ui::mojom::blink::CursorType cursor_type_ =
      ui::mojom::blink::CursorType::kPointer;
  bool is_custom_cursor_ = false;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_VIRTUAL_CURSOR_TOOL_H_
