// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/virtual_cursor_layer.h"

#include "cc/paint/display_item_list.h"
#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_recorder.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/gfx/geometry/rect.h"

namespace content {

namespace {

// Create the arrow cursor path (standard pointer)
SkPath CreateArrowPath(float scale) {
  SkPathBuilder builder;
  // Arrow pointing down-right, tip at top-left
  builder.moveTo(1 * scale, 1 * scale);
  builder.lineTo(1 * scale, 17 * scale);
  builder.lineTo(5 * scale, 13 * scale);
  builder.lineTo(8 * scale, 21 * scale);
  builder.lineTo(11 * scale, 20 * scale);
  builder.lineTo(8 * scale, 12 * scale);
  builder.lineTo(13 * scale, 12 * scale);
  builder.close();
  return builder.detach();
}

// Create the hand/pointer cursor path (for links)
SkPath CreateHandPath(float scale) {
  SkPathBuilder builder;
  // Pointing hand shape
  builder.moveTo(8 * scale, 1 * scale);
  builder.lineTo(8 * scale, 9 * scale);
  builder.lineTo(6 * scale, 9 * scale);
  builder.lineTo(6 * scale, 11 * scale);
  builder.lineTo(4 * scale, 11 * scale);
  builder.lineTo(4 * scale, 13 * scale);
  builder.lineTo(2 * scale, 13 * scale);
  builder.lineTo(2 * scale, 18 * scale);
  builder.lineTo(4 * scale, 20 * scale);
  builder.lineTo(14 * scale, 20 * scale);
  builder.lineTo(16 * scale, 18 * scale);
  builder.lineTo(16 * scale, 9 * scale);
  builder.lineTo(14 * scale, 9 * scale);
  builder.lineTo(14 * scale, 7 * scale);
  builder.lineTo(12 * scale, 7 * scale);
  builder.lineTo(12 * scale, 5 * scale);
  builder.lineTo(10 * scale, 5 * scale);
  builder.lineTo(10 * scale, 1 * scale);
  builder.close();
  return builder.detach();
}

// Create the I-beam/text cursor path
SkPath CreateIBeamPath(float scale) {
  SkPathBuilder builder;
  // Top serif
  builder.moveTo(6 * scale, 2 * scale);
  builder.lineTo(10 * scale, 2 * scale);
  builder.lineTo(10 * scale, 4 * scale);
  builder.lineTo(9 * scale, 4 * scale);
  builder.lineTo(9 * scale, 11 * scale);
  builder.lineTo(10 * scale, 11 * scale);
  builder.lineTo(10 * scale, 13 * scale);
  builder.lineTo(9 * scale, 13 * scale);
  builder.lineTo(9 * scale, 20 * scale);
  builder.lineTo(10 * scale, 20 * scale);
  builder.lineTo(10 * scale, 22 * scale);
  builder.lineTo(6 * scale, 22 * scale);
  builder.lineTo(6 * scale, 20 * scale);
  builder.lineTo(7 * scale, 20 * scale);
  builder.lineTo(7 * scale, 13 * scale);
  builder.lineTo(6 * scale, 13 * scale);
  builder.lineTo(6 * scale, 11 * scale);
  builder.lineTo(7 * scale, 11 * scale);
  builder.lineTo(7 * scale, 4 * scale);
  builder.lineTo(6 * scale, 4 * scale);
  builder.close();
  return builder.detach();
}

// Create the crosshair cursor path
SkPath CreateCrosshairPath(float scale) {
  SkPathBuilder builder;
  float center = 12 * scale;
  float size = 10 * scale;
  float thickness = 1 * scale;
  // Vertical line
  builder.moveTo(center - thickness, center - size);
  builder.lineTo(center + thickness, center - size);
  builder.lineTo(center + thickness, center + size);
  builder.lineTo(center - thickness, center + size);
  builder.close();
  // Horizontal line
  builder.moveTo(center - size, center - thickness);
  builder.lineTo(center + size, center - thickness);
  builder.lineTo(center + size, center + thickness);
  builder.lineTo(center - size, center + thickness);
  builder.close();
  return builder.detach();
}

// Create the move cursor path (four-way arrows)
SkPath CreateMovePath(float scale) {
  SkPathBuilder builder;
  float center = 12 * scale;
  // Up arrow
  builder.moveTo(center, 2 * scale);
  builder.lineTo(center + 4 * scale, 6 * scale);
  builder.lineTo(center + 2 * scale, 6 * scale);
  builder.lineTo(center + 2 * scale, 10 * scale);
  builder.lineTo(center - 2 * scale, 10 * scale);
  builder.lineTo(center - 2 * scale, 6 * scale);
  builder.lineTo(center - 4 * scale, 6 * scale);
  builder.close();
  // Down arrow
  builder.moveTo(center, 22 * scale);
  builder.lineTo(center - 4 * scale, 18 * scale);
  builder.lineTo(center - 2 * scale, 18 * scale);
  builder.lineTo(center - 2 * scale, 14 * scale);
  builder.lineTo(center + 2 * scale, 14 * scale);
  builder.lineTo(center + 2 * scale, 18 * scale);
  builder.lineTo(center + 4 * scale, 18 * scale);
  builder.close();
  // Left arrow
  builder.moveTo(2 * scale, center);
  builder.lineTo(6 * scale, center - 4 * scale);
  builder.lineTo(6 * scale, center - 2 * scale);
  builder.lineTo(10 * scale, center - 2 * scale);
  builder.lineTo(10 * scale, center + 2 * scale);
  builder.lineTo(6 * scale, center + 2 * scale);
  builder.lineTo(6 * scale, center + 4 * scale);
  builder.close();
  // Right arrow
  builder.moveTo(22 * scale, center);
  builder.lineTo(18 * scale, center + 4 * scale);
  builder.lineTo(18 * scale, center + 2 * scale);
  builder.lineTo(14 * scale, center + 2 * scale);
  builder.lineTo(14 * scale, center - 2 * scale);
  builder.lineTo(18 * scale, center - 2 * scale);
  builder.lineTo(18 * scale, center - 4 * scale);
  builder.close();
  return builder.detach();
}

// Create the not-allowed cursor path (circle with slash)
SkPath CreateNotAllowedPath(float scale) {
  SkPathBuilder builder;
  float center = 12 * scale;
  float outer_radius = 9 * scale;
  float inner_radius = 7 * scale;
  // Outer and inner circles for the ring
  builder.addCircle(center, center, outer_radius, SkPathDirection::kCW);
  builder.addCircle(center, center, inner_radius, SkPathDirection::kCCW);
  // Diagonal slash
  builder.moveTo(center - 5 * scale, center - 5 * scale);
  builder.lineTo(center - 3 * scale, center - 5 * scale);
  builder.lineTo(center + 5 * scale, center + 5 * scale);
  builder.lineTo(center + 3 * scale, center + 5 * scale);
  builder.close();
  return builder.detach();
}

// Create north-south resize cursor
SkPath CreateNsResizePath(float scale) {
  SkPathBuilder builder;
  float center = 12 * scale;
  // Up arrow
  builder.moveTo(center, 2 * scale);
  builder.lineTo(center + 5 * scale, 7 * scale);
  builder.lineTo(center + 2 * scale, 7 * scale);
  builder.lineTo(center + 2 * scale, 17 * scale);
  builder.lineTo(center + 5 * scale, 17 * scale);
  builder.lineTo(center, 22 * scale);
  builder.lineTo(center - 5 * scale, 17 * scale);
  builder.lineTo(center - 2 * scale, 17 * scale);
  builder.lineTo(center - 2 * scale, 7 * scale);
  builder.lineTo(center - 5 * scale, 7 * scale);
  builder.close();
  return builder.detach();
}

// Create east-west resize cursor
SkPath CreateEwResizePath(float scale) {
  SkPathBuilder builder;
  float center = 12 * scale;
  // Left arrow
  builder.moveTo(2 * scale, center);
  builder.lineTo(7 * scale, center - 5 * scale);
  builder.lineTo(7 * scale, center - 2 * scale);
  builder.lineTo(17 * scale, center - 2 * scale);
  builder.lineTo(17 * scale, center - 5 * scale);
  builder.lineTo(22 * scale, center);
  builder.lineTo(17 * scale, center + 5 * scale);
  builder.lineTo(17 * scale, center + 2 * scale);
  builder.lineTo(7 * scale, center + 2 * scale);
  builder.lineTo(7 * scale, center + 5 * scale);
  builder.close();
  return builder.detach();
}

// Create wait/hourglass cursor
SkPath CreateWaitPath(float scale) {
  SkPathBuilder builder;
  // Hourglass shape
  builder.moveTo(4 * scale, 2 * scale);
  builder.lineTo(20 * scale, 2 * scale);
  builder.lineTo(20 * scale, 4 * scale);
  builder.lineTo(14 * scale, 10 * scale);
  builder.lineTo(14 * scale, 14 * scale);
  builder.lineTo(20 * scale, 20 * scale);
  builder.lineTo(20 * scale, 22 * scale);
  builder.lineTo(4 * scale, 22 * scale);
  builder.lineTo(4 * scale, 20 * scale);
  builder.lineTo(10 * scale, 14 * scale);
  builder.lineTo(10 * scale, 10 * scale);
  builder.lineTo(4 * scale, 4 * scale);
  builder.close();
  return builder.detach();
}

// Get the hotspot offset for a cursor type
gfx::PointF GetCursorHotspot(ui::mojom::CursorType cursor_type) {
  switch (cursor_type) {
    case ui::mojom::CursorType::kPointer:
    case ui::mojom::CursorType::kNone:
      return gfx::PointF(1.0f, 1.0f);

    case ui::mojom::CursorType::kHand:
      return gfx::PointF(8.0f, 1.0f);

    case ui::mojom::CursorType::kIBeam:
      return gfx::PointF(8.0f, 12.0f);

    case ui::mojom::CursorType::kCross:
    case ui::mojom::CursorType::kMove:
    case ui::mojom::CursorType::kMiddlePanning:
    case ui::mojom::CursorType::kMiddlePanningVertical:
    case ui::mojom::CursorType::kMiddlePanningHorizontal:
    case ui::mojom::CursorType::kNotAllowed:
    case ui::mojom::CursorType::kNoDrop:
    case ui::mojom::CursorType::kNorthResize:
    case ui::mojom::CursorType::kSouthResize:
    case ui::mojom::CursorType::kNorthSouthResize:
    case ui::mojom::CursorType::kRowResize:
    case ui::mojom::CursorType::kEastResize:
    case ui::mojom::CursorType::kWestResize:
    case ui::mojom::CursorType::kEastWestResize:
    case ui::mojom::CursorType::kColumnResize:
    case ui::mojom::CursorType::kNorthEastResize:
    case ui::mojom::CursorType::kNorthWestResize:
    case ui::mojom::CursorType::kSouthEastResize:
    case ui::mojom::CursorType::kSouthWestResize:
    case ui::mojom::CursorType::kNorthEastSouthWestResize:
    case ui::mojom::CursorType::kNorthWestSouthEastResize:
    case ui::mojom::CursorType::kWait:
    case ui::mojom::CursorType::kProgress:
      return gfx::PointF(12.0f, 12.0f);

    default:
      return gfx::PointF(1.0f, 1.0f);
  }
}

void DrawCursorPath(cc::PaintCanvas* canvas,
                    const SkPath& path,
                    float scale) {
  // Draw white outline/stroke for visibility
  cc::PaintFlags stroke_flags;
  stroke_flags.setColor(SK_ColorWHITE);
  stroke_flags.setStyle(cc::PaintFlags::kStroke_Style);
  stroke_flags.setStrokeWidth(2.0f * scale);
  stroke_flags.setAntiAlias(true);
  canvas->drawPath(path, stroke_flags);

  // Draw black fill
  cc::PaintFlags fill_flags;
  fill_flags.setColor(SK_ColorBLACK);
  fill_flags.setStyle(cc::PaintFlags::kFill_Style);
  fill_flags.setAntiAlias(true);
  canvas->drawPath(path, fill_flags);
}

}  // namespace

VirtualCursorLayer::VirtualCursorLayer() : PictureLayer(this) {
  SetIsDrawable(true);
  SetHitTestable(false);
  // Set the bounds to the cursor size
  SetBounds(gfx::Size(kCursorSize, kCursorSize));
}

VirtualCursorLayer::~VirtualCursorLayer() = default;

void VirtualCursorLayer::SetPosition(float x, float y) {
  position_ = gfx::PointF(x, y);

  // Get the hotspot offset for the current cursor type
  gfx::PointF hotspot = GetCursorHotspot(cursor_type_);

  // Use transform for positioning (efficient, no repaint needed)
  gfx::Transform transform;
  transform.Translate(x - hotspot.x() * scale_, y - hotspot.y() * scale_);
  SetTransform(transform);
}

void VirtualCursorLayer::SetCursorType(ui::mojom::CursorType cursor_type) {
  if (cursor_type_ == cursor_type) {
    return;
  }

  cursor_type_ = cursor_type;

  // Update the position to account for new hotspot
  SetPosition(position_.x(), position_.y());

  // Request a repaint since the cursor shape changed
  SetNeedsDisplay();
}

void VirtualCursorLayer::SetCursorVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }

  visible_ = visible;
  SetHideLayerAndSubtree(!visible);
}

scoped_refptr<cc::DisplayItemList> VirtualCursorLayer::PaintContentsToDisplayList() {
  auto display_list = base::MakeRefCounted<cc::DisplayItemList>();

  display_list->StartPaint();

  cc::PaintRecorder recorder;
  cc::PaintCanvas* canvas = recorder.beginRecording();

  SkPath path;

  switch (cursor_type_) {
    case ui::mojom::CursorType::kPointer:
    case ui::mojom::CursorType::kNone:
      path = CreateArrowPath(scale_);
      break;

    case ui::mojom::CursorType::kHand:
      path = CreateHandPath(scale_);
      break;

    case ui::mojom::CursorType::kIBeam:
      path = CreateIBeamPath(scale_);
      break;

    case ui::mojom::CursorType::kCross:
      path = CreateCrosshairPath(scale_);
      break;

    case ui::mojom::CursorType::kMove:
    case ui::mojom::CursorType::kMiddlePanning:
    case ui::mojom::CursorType::kMiddlePanningVertical:
    case ui::mojom::CursorType::kMiddlePanningHorizontal:
      path = CreateMovePath(scale_);
      break;

    case ui::mojom::CursorType::kNotAllowed:
    case ui::mojom::CursorType::kNoDrop:
      path = CreateNotAllowedPath(scale_);
      break;

    case ui::mojom::CursorType::kNorthResize:
    case ui::mojom::CursorType::kSouthResize:
    case ui::mojom::CursorType::kNorthSouthResize:
    case ui::mojom::CursorType::kRowResize:
      path = CreateNsResizePath(scale_);
      break;

    case ui::mojom::CursorType::kEastResize:
    case ui::mojom::CursorType::kWestResize:
    case ui::mojom::CursorType::kEastWestResize:
    case ui::mojom::CursorType::kColumnResize:
      path = CreateEwResizePath(scale_);
      break;

    case ui::mojom::CursorType::kWait:
    case ui::mojom::CursorType::kProgress:
      path = CreateWaitPath(scale_);
      break;

    default:
      path = CreateArrowPath(scale_);
      break;
  }

  DrawCursorPath(canvas, path, scale_);

  display_list->push<cc::DrawRecordOp>(recorder.finishRecordingAsPicture());
  display_list->EndPaintOfUnpaired(gfx::Rect(kCursorSize, kCursorSize));
  display_list->Finalize();

  return display_list;
}

bool VirtualCursorLayer::FillsBoundsCompletely() const {
  return false;
}

}  // namespace content
