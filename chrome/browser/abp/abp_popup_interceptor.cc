// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_popup_interceptor.h"

#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/abp/abp_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace abp {

PendingSelectPopup::PendingSelectPopup() = default;
PendingSelectPopup::~PendingSelectPopup() = default;
PendingSelectPopup::PendingSelectPopup(PendingSelectPopup&&) = default;
PendingSelectPopup& PendingSelectPopup::operator=(PendingSelectPopup&&) =
    default;

PendingDateTimePopup::PendingDateTimePopup() = default;
PendingDateTimePopup::~PendingDateTimePopup() = default;
PendingDateTimePopup::PendingDateTimePopup(PendingDateTimePopup&&) = default;
PendingDateTimePopup& PendingDateTimePopup::operator=(
    PendingDateTimePopup&&) = default;

AbpPopupInterceptor::AbpPopupInterceptor(AbpController* controller)
    : controller_(controller) {}

AbpPopupInterceptor::~AbpPopupInterceptor() = default;

std::string AbpPopupInterceptor::GenerateSelectPopupId() {
  return base::StringPrintf("sp_%d", next_select_popup_id_++);
}

bool AbpPopupInterceptor::OnSelectPopupRequested(
    content::RenderFrameHost* rfh,
    mojo::PendingRemote<blink::mojom::PopupMenuClient> popup_client,
    const gfx::Rect& bounds,
    int32_t selected_item,
    std::vector<blink::mojom::MenuItemPtr> menu_items,
    bool allow_multiple_selection) {
  if (!controller_)
    return false;

  // Get tab_id from the DevToolsAgentHost associated with this frame's
  // WebContents (ABP uses DevToolsAgentHost IDs as tab IDs)
  std::string tab_id;
  if (rfh) {
    auto* wc = content::WebContents::FromRenderFrameHost(rfh);
    if (wc) {
      tab_id = controller_->GetTabIdForWebContents(wc);
    }
  }
  if (tab_id.empty())
    return false;

  std::string popup_id = GenerateSelectPopupId();

  VLOG(1) << "ABP: Select popup intercepted, id=" << popup_id
          << " tab=" << tab_id
          << " items=" << menu_items.size();

  // Serialize event data before moving items
  base::Value::Dict event_data;
  event_data.Set("type", "select_open");
  event_data.Set("id", popup_id);
  event_data.Set("tab_id", tab_id);
  event_data.Set("allow_multiple_selection", allow_multiple_selection);
  event_data.Set("selected_index", selected_item);

  base::Value::Dict bounds_dict;
  bounds_dict.Set("x", bounds.x());
  bounds_dict.Set("y", bounds.y());
  bounds_dict.Set("width", bounds.width());
  bounds_dict.Set("height", bounds.height());
  event_data.Set("bounds", std::move(bounds_dict));

  event_data.Set("items", SerializeMenuItems(menu_items));

  // Store pending state
  PendingSelectPopup pending;
  pending.tab_id = tab_id;
  pending.client.Bind(std::move(popup_client));
  pending.items = std::move(menu_items);
  pending.selected_index = selected_item;
  pending.allow_multiple = allow_multiple_selection;
  pending.bounds = bounds;

  // Set disconnect handler to clean up if renderer goes away
  pending.client.set_disconnect_handler(base::BindOnce(
      [](AbpPopupInterceptor* self, std::string id) {
        VLOG(1) << "ABP: Select popup " << id << " disconnected";
        self->pending_select_popups_.erase(id);
      },
      base::Unretained(this), popup_id));

  pending_select_popups_[popup_id] = std::move(pending);

  // Emit event (collected by event collector for action responses)
  controller_->EmitPopupEvent("select_open", std::move(event_data));

  return true;  // Intercepted — suppress native UI
}

bool AbpPopupInterceptor::RespondToSelectPopup(
    const std::string& popup_id,
    const std::vector<int32_t>& indices) {
  auto it = pending_select_popups_.find(popup_id);
  if (it == pending_select_popups_.end())
    return false;

  it->second.client->DidAcceptIndices(indices);
  pending_select_popups_.erase(it);
  return true;
}

bool AbpPopupInterceptor::CancelSelectPopup(const std::string& popup_id) {
  auto it = pending_select_popups_.find(popup_id);
  if (it == pending_select_popups_.end())
    return false;

  it->second.client->DidCancel();
  pending_select_popups_.erase(it);
  return true;
}

base::Value::Dict AbpPopupInterceptor::GetPendingSelectPopup(
    const std::string& popup_id) const {
  auto it = pending_select_popups_.find(popup_id);
  if (it == pending_select_popups_.end())
    return base::Value::Dict();

  base::Value::Dict result;
  result.Set("type", "select_open");
  result.Set("id", popup_id);
  result.Set("tab_id", it->second.tab_id);
  result.Set("allow_multiple_selection", it->second.allow_multiple);
  result.Set("selected_index", it->second.selected_index);

  base::Value::Dict bounds;
  bounds.Set("x", it->second.bounds.x());
  bounds.Set("y", it->second.bounds.y());
  bounds.Set("width", it->second.bounds.width());
  bounds.Set("height", it->second.bounds.height());
  result.Set("bounds", std::move(bounds));

  result.Set("items", SerializeMenuItems(it->second.items));
  return result;
}

void AbpPopupInterceptor::CleanupForTab(const std::string& tab_id) {
  for (auto it = pending_select_popups_.begin();
       it != pending_select_popups_.end();) {
    if (it->second.tab_id == tab_id) {
      it = pending_select_popups_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = pending_datetime_popups_.begin();
       it != pending_datetime_popups_.end();) {
    if (it->second.tab_id == tab_id) {
      it = pending_datetime_popups_.erase(it);
    } else {
      ++it;
    }
  }
}

std::string AbpPopupInterceptor::GenerateDateTimePopupId() {
  return base::StringPrintf("dtp_%d", next_datetime_popup_id_++);
}

bool AbpPopupInterceptor::OnDateTimePopupRequested(
    content::RenderFrameHost* rfh,
    mojo::PendingRemote<blink::mojom::DateTimePopupClient> client,
    blink::mojom::DateTimePopupParamsPtr params) {
  if (!controller_)
    return false;

  std::string tab_id;
  if (rfh) {
    auto* wc = content::WebContents::FromRenderFrameHost(rfh);
    if (wc) {
      tab_id = controller_->GetTabIdForWebContents(wc);
    }
  }
  if (tab_id.empty())
    return false;

  std::string popup_id = GenerateDateTimePopupId();

  VLOG(1) << "ABP: Date/time popup intercepted, id=" << popup_id
          << " tab=" << tab_id
          << " input_type=" << params->input_type;

  base::Value::Dict event_data;
  event_data.Set("type", "datetime_picker_open");
  event_data.Set("id", popup_id);
  event_data.Set("tab_id", tab_id);
  event_data.Set("input_type", params->input_type);
  event_data.Set("value", params->value);
  event_data.Set("min", params->min);
  event_data.Set("max", params->max);
  event_data.Set("step", params->step);

  base::Value::Dict bounds_dict;
  bounds_dict.Set("x", params->bounds.x());
  bounds_dict.Set("y", params->bounds.y());
  bounds_dict.Set("width", params->bounds.width());
  bounds_dict.Set("height", params->bounds.height());
  event_data.Set("bounds", std::move(bounds_dict));

  PendingDateTimePopup pending;
  pending.tab_id = tab_id;
  pending.client.Bind(std::move(client));
  pending.params = std::move(params);

  // Set disconnect handler to clean up if renderer goes away
  pending.client.set_disconnect_handler(base::BindOnce(
      [](AbpPopupInterceptor* self, std::string id) {
        VLOG(1) << "ABP: Date/time popup " << id << " disconnected";
        self->pending_datetime_popups_.erase(id);
      },
      base::Unretained(this), popup_id));

  pending_datetime_popups_[popup_id] = std::move(pending);

  controller_->EmitPopupEvent("datetime_picker_open", std::move(event_data));

  return true;  // Intercepted — suppress native UI
}

bool AbpPopupInterceptor::RespondToDateTimePopup(
    const std::string& popup_id,
    const std::string& iso_value) {
  auto it = pending_datetime_popups_.find(popup_id);
  if (it == pending_datetime_popups_.end())
    return false;

  it->second.client->DidChooseValue(iso_value);
  pending_datetime_popups_.erase(it);
  return true;
}

bool AbpPopupInterceptor::CancelDateTimePopup(const std::string& popup_id) {
  auto it = pending_datetime_popups_.find(popup_id);
  if (it == pending_datetime_popups_.end())
    return false;

  it->second.client->DidCancel();
  pending_datetime_popups_.erase(it);
  return true;
}

base::Value::Dict AbpPopupInterceptor::GetPendingDateTimePopup(
    const std::string& popup_id) const {
  auto it = pending_datetime_popups_.find(popup_id);
  if (it == pending_datetime_popups_.end())
    return base::Value::Dict();

  base::Value::Dict result;
  result.Set("type", "datetime_picker_open");
  result.Set("id", popup_id);
  result.Set("tab_id", it->second.tab_id);
  result.Set("input_type", it->second.params->input_type);
  result.Set("value", it->second.params->value);
  result.Set("min", it->second.params->min);
  result.Set("max", it->second.params->max);
  result.Set("step", it->second.params->step);

  base::Value::Dict bounds;
  bounds.Set("x", it->second.params->bounds.x());
  bounds.Set("y", it->second.params->bounds.y());
  bounds.Set("width", it->second.params->bounds.width());
  bounds.Set("height", it->second.params->bounds.height());
  result.Set("bounds", std::move(bounds));

  return result;
}

// static
base::Value::List AbpPopupInterceptor::SerializeMenuItems(
    const std::vector<blink::mojom::MenuItemPtr>& items) {
  base::Value::List list;
  int index = 0;
  for (const auto& item : items) {
    base::Value::Dict d;
    d.Set("index", index++);

    switch (item->type) {
      case blink::mojom::MenuItem::Type::kOption:
        d.Set("type", "option");
        break;
      case blink::mojom::MenuItem::Type::kCheckableOption:
        d.Set("type", "checkable_option");
        break;
      case blink::mojom::MenuItem::Type::kGroup:
        d.Set("type", "group");
        break;
      case blink::mojom::MenuItem::Type::kSeparator:
        d.Set("type", "separator");
        break;
      case blink::mojom::MenuItem::Type::kSubMenu:
        d.Set("type", "submenu");
        break;
    }

    if (item->label.has_value() && !item->label->empty()) {
      d.Set("label", *item->label);
    }
    if (item->tool_tip.has_value() && !item->tool_tip->empty()) {
      d.Set("tool_tip", *item->tool_tip);
    }
    d.Set("enabled", item->enabled);
    d.Set("checked", item->checked);

    list.Append(std::move(d));
  }
  return list;
}

}  // namespace abp
