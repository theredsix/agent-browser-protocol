// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_POPUP_INTERCEPTOR_H_
#define CONTENT_PUBLIC_BROWSER_POPUP_INTERCEPTOR_H_

#include <vector>

#include "content/common/content_export.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/mojom/choosers/date_time_popup.mojom-forward.h"
#include "third_party/blink/public/mojom/choosers/popup_menu.mojom-forward.h"
#include "ui/gfx/geometry/rect.h"

namespace content {

class RenderFrameHost;

// Interface for intercepting native popups before platform UI is shown.
// Embedders (e.g., ABP agent browser protocol) implement this to capture
// select dropdowns, exposing them to external agents.
class CONTENT_EXPORT PopupInterceptor {
 public:
  virtual ~PopupInterceptor() = default;

  // Called when a <select> element requests a native popup menu.
  // Returns true if the popup was intercepted (native UI is suppressed).
  // The interceptor takes ownership of |popup_client| to send the selection.
  virtual bool OnSelectPopupRequested(
      RenderFrameHost* rfh,
      mojo::PendingRemote<blink::mojom::PopupMenuClient> popup_client,
      const gfx::Rect& bounds,
      int32_t selected_item,
      std::vector<blink::mojom::MenuItemPtr> menu_items,
      bool allow_multiple_selection) = 0;

  // Called when a date/time <input> requests its picker. Returns true if
  // intercepted (native picker suppressed). The interceptor takes ownership of
  // |client| to deliver the chosen ISO value (or cancel).
  virtual bool OnDateTimePopupRequested(
      RenderFrameHost* rfh,
      mojo::PendingRemote<blink::mojom::DateTimePopupClient> client,
      blink::mojom::DateTimePopupParamsPtr params) = 0;
};

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_POPUP_INTERCEPTOR_H_
