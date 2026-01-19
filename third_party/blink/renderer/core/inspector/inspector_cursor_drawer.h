// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_CURSOR_DRAWER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_CURSOR_DRAWER_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-blink.h"

namespace cc {
class PaintCanvas;
}

namespace gfx {
class PointF;
}

namespace blink {

// Draws cursor icons for the virtual cursor overlay in inspector.
// This class provides static methods to draw various cursor types
// using Skia paths, with support for high-DPI rendering.
class CORE_EXPORT InspectorCursorDrawer {
 public:
  // Draws a cursor icon at the specified position on the canvas.
  // The cursor type determines which icon shape is drawn.
  // |position| is in the same coordinate space as the canvas (CSS pixels).
  // |scale| is used to scale the cursor appropriately for the DPI.
  static void DrawCursor(cc::PaintCanvas* canvas,
                         ui::mojom::blink::CursorType cursor_type,
                         const gfx::PointF& position,
                         float scale);

  // Converts a cursor type to its string name for protocol responses.
  static String CursorTypeName(ui::mojom::blink::CursorType cursor_type);

  // Get the hotspot offset for a cursor type (where the "click point" is).
  // Returns offset from top-left of cursor image in DIPs.
  static gfx::PointF GetCursorHotspot(ui::mojom::blink::CursorType cursor_type);

 private:
  InspectorCursorDrawer() = delete;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_CURSOR_DRAWER_H_
