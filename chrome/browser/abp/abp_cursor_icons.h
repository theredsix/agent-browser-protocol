#ifndef CHROME_BROWSER_ABP_ABP_CURSOR_ICONS_H_
#define CHROME_BROWSER_ABP_ABP_CURSOR_ICONS_H_

#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPath.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-forward.h"

namespace gfx {
class PointF;
}

namespace abp {

// Draws a cursor icon at the specified position on the canvas.
// The cursor type determines which icon shape is drawn.
// |position| is in the same coordinate space as the canvas.
// |scale| is used to scale the cursor appropriately for the DPI.
void DrawCursorIcon(SkCanvas* canvas,
                    ui::mojom::CursorType cursor_type,
                    const gfx::PointF& position,
                    float scale);

// Get the hotspot offset for a cursor type (where the "click point" is)
// Returns offset from top-left of cursor image in DIPs
gfx::PointF GetCursorHotspot(ui::mojom::CursorType cursor_type);

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_CURSOR_ICONS_H_
