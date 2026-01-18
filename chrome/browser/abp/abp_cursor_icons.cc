#include "chrome/browser/abp/abp_cursor_icons.h"

#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/cursor/mojom/cursor_type.mojom.h"
#include "ui/gfx/geometry/point_f.h"

namespace abp {

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
  // Outer circle using addOval equivalent - just draw the ring
  // Draw a simple ring by creating outer and inner circles
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

void DrawCursorPath(SkCanvas* canvas,
                    const SkPath& path,
                    const gfx::PointF& position,
                    float scale) {
  canvas->save();
  canvas->translate(position.x(), position.y());

  // Draw white outline/stroke for visibility
  SkPaint stroke_paint;
  stroke_paint.setColor(SK_ColorWHITE);
  stroke_paint.setStyle(SkPaint::kStroke_Style);
  stroke_paint.setStrokeWidth(2.0f * scale);
  stroke_paint.setAntiAlias(true);
  canvas->drawPath(path, stroke_paint);

  // Draw black fill
  SkPaint fill_paint;
  fill_paint.setColor(SK_ColorBLACK);
  fill_paint.setStyle(SkPaint::kFill_Style);
  fill_paint.setAntiAlias(true);
  canvas->drawPath(path, fill_paint);

  canvas->restore();
}

}  // namespace

void DrawCursorIcon(SkCanvas* canvas,
                    ui::mojom::CursorType cursor_type,
                    const gfx::PointF& position,
                    float scale) {
  if (!canvas) {
    return;
  }

  // Get the hotspot and adjust position so hotspot is at the click point
  gfx::PointF hotspot = GetCursorHotspot(cursor_type);
  gfx::PointF adjusted_pos(position.x() - hotspot.x() * scale,
                           position.y() - hotspot.y() * scale);

  SkPath path;

  switch (cursor_type) {
    case ui::mojom::CursorType::kPointer:
    case ui::mojom::CursorType::kNone:
      path = CreateArrowPath(scale);
      break;

    case ui::mojom::CursorType::kHand:
      path = CreateHandPath(scale);
      break;

    case ui::mojom::CursorType::kIBeam:
      path = CreateIBeamPath(scale);
      break;

    case ui::mojom::CursorType::kCross:
      path = CreateCrosshairPath(scale);
      break;

    case ui::mojom::CursorType::kMove:
    case ui::mojom::CursorType::kMiddlePanning:
    case ui::mojom::CursorType::kMiddlePanningVertical:
    case ui::mojom::CursorType::kMiddlePanningHorizontal:
      path = CreateMovePath(scale);
      break;

    case ui::mojom::CursorType::kNotAllowed:
    case ui::mojom::CursorType::kNoDrop:
      path = CreateNotAllowedPath(scale);
      break;

    case ui::mojom::CursorType::kNorthResize:
    case ui::mojom::CursorType::kSouthResize:
    case ui::mojom::CursorType::kNorthSouthResize:
    case ui::mojom::CursorType::kRowResize:
      path = CreateNsResizePath(scale);
      break;

    case ui::mojom::CursorType::kEastResize:
    case ui::mojom::CursorType::kWestResize:
    case ui::mojom::CursorType::kEastWestResize:
    case ui::mojom::CursorType::kColumnResize:
      path = CreateEwResizePath(scale);
      break;

    case ui::mojom::CursorType::kWait:
    case ui::mojom::CursorType::kProgress:
      path = CreateWaitPath(scale);
      break;

    // For other cursor types, fall back to arrow
    default:
      path = CreateArrowPath(scale);
      break;
  }

  DrawCursorPath(canvas, path, adjusted_pos, scale);
}

gfx::PointF GetCursorHotspot(ui::mojom::CursorType cursor_type) {
  switch (cursor_type) {
    case ui::mojom::CursorType::kPointer:
    case ui::mojom::CursorType::kNone:
      // Arrow tip is at top-left
      return gfx::PointF(1.0f, 1.0f);

    case ui::mojom::CursorType::kHand:
      // Fingertip is at top of index finger
      return gfx::PointF(8.0f, 1.0f);

    case ui::mojom::CursorType::kIBeam:
      // Center of I-beam
      return gfx::PointF(8.0f, 12.0f);

    case ui::mojom::CursorType::kCross:
      // Center of crosshair
      return gfx::PointF(12.0f, 12.0f);

    case ui::mojom::CursorType::kMove:
    case ui::mojom::CursorType::kMiddlePanning:
    case ui::mojom::CursorType::kMiddlePanningVertical:
    case ui::mojom::CursorType::kMiddlePanningHorizontal:
      // Center of four-way arrows
      return gfx::PointF(12.0f, 12.0f);

    case ui::mojom::CursorType::kNotAllowed:
    case ui::mojom::CursorType::kNoDrop:
      // Center of circle
      return gfx::PointF(12.0f, 12.0f);

    case ui::mojom::CursorType::kNorthResize:
    case ui::mojom::CursorType::kSouthResize:
    case ui::mojom::CursorType::kNorthSouthResize:
    case ui::mojom::CursorType::kRowResize:
    case ui::mojom::CursorType::kEastResize:
    case ui::mojom::CursorType::kWestResize:
    case ui::mojom::CursorType::kEastWestResize:
    case ui::mojom::CursorType::kColumnResize:
      // Center of resize arrows
      return gfx::PointF(12.0f, 12.0f);

    case ui::mojom::CursorType::kWait:
    case ui::mojom::CursorType::kProgress:
      // Center of hourglass
      return gfx::PointF(12.0f, 12.0f);

    default:
      // Default to arrow hotspot
      return gfx::PointF(1.0f, 1.0f);
  }
}

}  // namespace abp
