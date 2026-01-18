#include "chrome/browser/abp/abp_mouse_tracker.h"

#import <Cocoa/Cocoa.h>

#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"

namespace abp {

gfx::PointF AbpMouseTracker::GetLastMousePositionInScreen() {
  // Get the current mouse location in screen coordinates
  // NSEvent.mouseLocation returns position in screen coordinates
  // with origin at bottom-left of primary screen
  NSPoint mouse_loc = [NSEvent mouseLocation];

  // Convert from Cocoa coordinates (origin at bottom-left) to
  // Chromium coordinates (origin at top-left)
  NSScreen* primary_screen = [[NSScreen screens] firstObject];
  if (!primary_screen) {
    return gfx::PointF(-1, -1);
  }

  CGFloat screen_height = [primary_screen frame].size.height;
  return gfx::PointF(mouse_loc.x, screen_height - mouse_loc.y);
}

gfx::PointF AbpMouseTracker::GetMousePositionInView(
    content::WebContents* web_contents) {
  if (!web_contents) {
    return gfx::PointF(-1, -1);
  }

  content::RenderWidgetHostView* rwhv =
      web_contents->GetRenderWidgetHostView();
  if (!rwhv) {
    return gfx::PointF(-1, -1);
  }

  // Get mouse position in screen coordinates (Chromium coordinate system)
  gfx::PointF screen_pos = GetLastMousePositionInScreen();
  if (screen_pos.x() < 0) {
    return gfx::PointF(-1, -1);
  }

  // Get the view bounds in screen coordinates
  gfx::Rect view_bounds = rwhv->GetViewBounds();

  // Convert screen coordinates to view-local coordinates
  float local_x = screen_pos.x() - view_bounds.x();
  float local_y = screen_pos.y() - view_bounds.y();

  // Check if mouse is within the view bounds
  if (local_x < 0 || local_y < 0 ||
      local_x >= view_bounds.width() || local_y >= view_bounds.height()) {
    return gfx::PointF(-1, -1);
  }

  // Convert to CSS pixels by dividing by device scale factor
  float scale = rwhv->GetDeviceScaleFactor();
  return gfx::PointF(local_x / scale, local_y / scale);
}

}  // namespace abp
