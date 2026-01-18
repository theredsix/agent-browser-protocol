#include "chrome/browser/abp/abp_mouse_tracker.h"

#include "base/logging.h"
#include "build/build_config.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/point_conversions.h"
#include "ui/gfx/geometry/point_f.h"

#if !BUILDFLAG(IS_MAC)
#include "ui/aura/env.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#endif

namespace abp {

namespace {
AbpMouseTracker* g_mouse_tracker = nullptr;
}

AbpMouseTracker::AbpMouseTracker() = default;
AbpMouseTracker::~AbpMouseTracker() = default;

#if !BUILDFLAG(IS_MAC)

gfx::PointF AbpMouseTracker::GetLastMousePositionInScreen() {
  if (!aura::Env::HasInstance()) {
    return gfx::PointF(-1, -1);
  }

  gfx::Point screen_pos = aura::Env::GetInstance()->last_mouse_location();
  return gfx::PointF(screen_pos);
}

gfx::PointF AbpMouseTracker::GetMousePositionInView(
    content::WebContents* web_contents) {
  if (!web_contents) {
    LOG(INFO) << "ABP MouseTracker: No web_contents";
    return gfx::PointF(-1, -1);
  }

  content::RenderWidgetHostView* rwhv =
      web_contents->GetRenderWidgetHostView();
  if (!rwhv) {
    LOG(INFO) << "ABP MouseTracker: No RWHV";
    return gfx::PointF(-1, -1);
  }

  if (!aura::Env::HasInstance()) {
    LOG(INFO) << "ABP MouseTracker: No aura::Env instance";
    return gfx::PointF(-1, -1);
  }

  // Get mouse position in screen coordinates
  gfx::Point screen_pos = aura::Env::GetInstance()->last_mouse_location();

  // Get the view bounds to check if mouse is inside
  gfx::Rect view_bounds = rwhv->GetViewBounds();

  LOG(INFO) << "ABP MouseTracker: screen_pos=(" << screen_pos.x() << ","
            << screen_pos.y() << ") view_bounds=" << view_bounds.ToString();

  // Convert from screen to local view coordinates
  gfx::Point local_pos = screen_pos;
  local_pos.Offset(-view_bounds.x(), -view_bounds.y());

  // Check if it's within the view's local bounds
  gfx::Rect local_bounds(0, 0, view_bounds.width(), view_bounds.height());

  if (!local_bounds.Contains(local_pos)) {
    // Mouse is outside the view
    LOG(INFO) << "ABP MouseTracker: Mouse outside view, local_pos=("
              << local_pos.x() << "," << local_pos.y() << ")";
    return gfx::PointF(-1, -1);
  }

  // Convert to CSS pixels by dividing by device scale factor
  float scale = rwhv->GetDeviceScaleFactor();
  gfx::PointF result(local_pos.x() / scale, local_pos.y() / scale);

  LOG(INFO) << "ABP MouseTracker: Mouse inside view at CSS pixels ("
            << result.x() << "," << result.y() << ")";
  return result;
}

#endif  // !BUILDFLAG(IS_MAC)

AbpMouseTracker* GetMouseTracker() {
  if (!g_mouse_tracker) {
    g_mouse_tracker = new AbpMouseTracker();
  }
  return g_mouse_tracker;
}

}  // namespace abp
