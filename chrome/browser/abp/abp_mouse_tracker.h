#ifndef CHROME_BROWSER_ABP_ABP_MOUSE_TRACKER_H_
#define CHROME_BROWSER_ABP_ABP_MOUSE_TRACKER_H_

#include "build/build_config.h"
#include "ui/gfx/geometry/point_f.h"

namespace content {
class WebContents;
}

namespace abp {

// Cross-platform mouse position tracking for ABP screenshots.
// Uses aura::Env on Windows/Linux/ChromeOS and Cocoa on Mac.
class AbpMouseTracker {
 public:
  AbpMouseTracker();
  ~AbpMouseTracker();

  AbpMouseTracker(const AbpMouseTracker&) = delete;
  AbpMouseTracker& operator=(const AbpMouseTracker&) = delete;

  // Get the last known mouse position in screen coordinates.
  // Returns the position, or (-1, -1) if unknown.
  gfx::PointF GetLastMousePositionInScreen();

  // Get the last known mouse position relative to a WebContents view.
  // Returns the position in CSS pixels, or (-1, -1) if unknown or
  // the mouse is outside the view.
  gfx::PointF GetMousePositionInView(content::WebContents* web_contents);

 private:
#if BUILDFLAG(IS_MAC)
  // Mac-specific tracking state would go here if needed
#endif
};

// Global singleton accessor
AbpMouseTracker* GetMouseTracker();

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_MOUSE_TRACKER_H_
