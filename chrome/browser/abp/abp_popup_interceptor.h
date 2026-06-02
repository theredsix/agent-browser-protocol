// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_POPUP_INTERCEPTOR_H_
#define CHROME_BROWSER_ABP_ABP_POPUP_INTERCEPTOR_H_

#include <map>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "content/public/browser/popup_interceptor.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/mojom/choosers/date_time_popup.mojom.h"
#include "third_party/blink/public/mojom/choosers/popup_menu.mojom.h"

namespace abp {

class AbpController;

struct PendingSelectPopup {
  PendingSelectPopup();
  ~PendingSelectPopup();
  PendingSelectPopup(PendingSelectPopup&&);
  PendingSelectPopup& operator=(PendingSelectPopup&&);

  std::string tab_id;
  mojo::Remote<blink::mojom::PopupMenuClient> client;
  std::vector<blink::mojom::MenuItemPtr> items;
  int32_t selected_index = 0;
  bool allow_multiple = false;
  gfx::Rect bounds;
};

struct PendingDateTimePopup {
  PendingDateTimePopup();
  ~PendingDateTimePopup();
  PendingDateTimePopup(PendingDateTimePopup&&);
  PendingDateTimePopup& operator=(PendingDateTimePopup&&);

  std::string tab_id;
  mojo::Remote<blink::mojom::DateTimePopupClient> client;
  blink::mojom::DateTimePopupParamsPtr params;
};

class AbpPopupInterceptor : public content::PopupInterceptor {
 public:
  explicit AbpPopupInterceptor(AbpController* controller);
  ~AbpPopupInterceptor() override;

  // content::PopupInterceptor:
  bool OnSelectPopupRequested(
      content::RenderFrameHost* rfh,
      mojo::PendingRemote<blink::mojom::PopupMenuClient> popup_client,
      const gfx::Rect& bounds,
      int32_t selected_item,
      std::vector<blink::mojom::MenuItemPtr> menu_items,
      bool allow_multiple_selection) override;

  // Respond to a pending select popup (called by controller on agent response)
  // Returns false if popup_id not found.
  bool RespondToSelectPopup(const std::string& popup_id,
                            const std::vector<int32_t>& indices);
  bool CancelSelectPopup(const std::string& popup_id);

  // Get pending popup info as JSON (for REST/MCP)
  base::Value::Dict GetPendingSelectPopup(const std::string& popup_id) const;

  // content::PopupInterceptor:
  bool OnDateTimePopupRequested(
      content::RenderFrameHost* rfh,
      mojo::PendingRemote<blink::mojom::DateTimePopupClient> client,
      blink::mojom::DateTimePopupParamsPtr params) override;

  // Respond to a pending date/time popup. Returns false if popup_id not found.
  bool RespondToDateTimePopup(const std::string& popup_id,
                              const std::string& iso_value);
  bool CancelDateTimePopup(const std::string& popup_id);

  // Get pending date/time popup info as JSON (for REST/MCP)
  base::Value::Dict GetPendingDateTimePopup(const std::string& popup_id) const;

  // Clean up popups for a tab (called on tab close)
  void CleanupForTab(const std::string& tab_id);

 private:
  std::string GenerateSelectPopupId();
  std::string GenerateDateTimePopupId();

  // Serialize menu items to JSON
  static base::Value::List SerializeMenuItems(
      const std::vector<blink::mojom::MenuItemPtr>& items);

  raw_ptr<AbpController> controller_;
  std::map<std::string, PendingSelectPopup> pending_select_popups_;
  int next_select_popup_id_ = 1;
  std::map<std::string, PendingDateTimePopup> pending_datetime_popups_;
  int next_datetime_popup_id_ = 1;
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_POPUP_INTERCEPTOR_H_
