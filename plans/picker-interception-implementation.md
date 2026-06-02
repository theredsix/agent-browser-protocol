# Picker Interception Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make ABP capture `<select>` popups on Linux/Windows, and intercept the date/time input family (date/time/datetime-local/month/week) so an agent gets a `datetime_picker_open` event plus a respond action.

**Architecture:** Part A flips the `use_external_popup_menu` build flag so Linux/Windows route `<select>` through the existing browser-side interceptor. Part B adds a new browser→renderer mojo (`DateTimePopupClient`) + a `LocalFrameHost.ShowDateTimePopup` method, hooks `ChromeClientImpl::OpenDateTimeChooser` with a new `AbpDateTimeChooser` (modeled on `ExternalDateTimeChooser`), routes it to a new `content::PopupInterceptor::OnDateTimePopupRequested`, and surfaces it through `AbpPopupInterceptor` exactly like the select popup. Agent values are ISO strings; Blink applies them via `DateTimeChooserClient::DidChooseValue(const String&)`.

**Tech Stack:** Chromium (C++), Mojo IPC, Blink renderer, ABP REST/MCP layer.

**Reference files to mirror (read before implementing):**
- Select interceptor: `chrome/browser/abp/abp_popup_interceptor.{h,cc}`
- Select REST handler: `AbpController::HandleSelectPopup` (`chrome/browser/abp/abp_controller.cc`)
- Browser hook: `RenderFrameHostImpl::ShowPopupMenu` (`render_frame_host_impl.cc:9310`)
- Renderer chooser model: `third_party/blink/renderer/core/html/forms/external_date_time_chooser.{h,cc}`
- Renderer hook: `ChromeClientImpl::OpenDateTimeChooser` (`chrome_client_impl.cc:809`)
- Client overloads: `DateTimeChooserClient::DidChooseValue(const String&)` (`date_time_chooser_client.h:49`)

**Build note:** Every task that touches `content/`, mojom, or Blink requires `autoninja -C out/Default chrome`. The executable output of this fork is `out/Default/abp`, NOT `out/Default/chrome` (`chrome/BUILD.gn` sets `_chrome_output_name = "abp"`); `out/Default/chrome` is a stale leftover that crashes. Build dir is `/home/paladin/src/src/out/Default`. Headless launch needs `--disable-gpu` (software fallback); never `pkill -f` the binary path (it self-matches the launcher shell). Intermediate tasks verify by **successful compile**; behavior is verified by the launch test in Task B8. A full build is multi-hour in this environment.

---

## Part A — `<select>` on Linux/Windows

### Task A1: Enable external popup menus on desktop Linux/Windows

**Files:**
- Modify: `content/public/common/features.gni:9-10`

- [ ] **Step 1: Make the edit** (already present in the working tree; confirm it matches)

```gn
  # Whether or not to use external popup menu.
  #
  # ABP: also enable on desktop Linux and Windows so the ABP popup interceptor
  # (content::PopupInterceptor, wired in RenderFrameHostImpl::ShowPopupMenu) can
  # capture <select> dropdowns. Without this, Blink renders those popups as
  # internal WebPagePopups that never reach the browser-side ShowPopupMenu hook,
  # so the agent never sees the choices or the select_open event.
  use_external_popup_menu = is_android || (is_apple && use_blink) ||
                            ((is_linux || is_win) && use_blink)
```

- [ ] **Step 2: Regenerate + confirm the flag value flipped**

Run:
```bash
cd /home/paladin/src/src && gn gen out/Default >/dev/null && \
  grep USE_EXTERNAL_POPUP_MENU out/Default/gen/third_party/blink/renderer/core/buildflags.h
```
Expected: `#define BUILDFLAG_INTERNAL_USE_EXTERNAL_POPUP_MENU() (1)`

- [ ] **Step 3: Commit**

```bash
git add content/public/common/features.gni
git commit -m "abp: route <select> popups through the interceptor on Linux/Windows"
```

Behavior is validated together with Part B in Task B8 (a `<select>` on the test page must now emit `select_open` on Linux). No ABP code change is needed — `OnSelectPopupRequested` is reused as-is.

---

## Part B — Date/time picker interception

### Task B1: New mojom — `DateTimePopupClient` + `ShowDateTimePopup`

**Files:**
- Create: `third_party/blink/public/mojom/choosers/date_time_popup.mojom`
- Modify: `third_party/blink/public/mojom/BUILD.gn` (add the new file to the `mojom` sources list, next to `choosers/date_time_chooser.mojom`)
- Modify: `third_party/blink/public/mojom/frame/frame.mojom` (add import + method on `LocalFrameHost`, next to `ShowPopupMenu` at line 545)

- [ ] **Step 1: Create the mojom**

`third_party/blink/public/mojom/choosers/date_time_popup.mojom`:
```mojom
// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

module blink.mojom;

import "third_party/blink/public/mojom/choosers/date_time_chooser.mojom";
import "ui/gfx/geometry/mojom/geometry.mojom";

// Parameters describing a date/time picker the agent should respond to.
// All values are ISO strings in the input element's own format (e.g.
// "2026-01-15" for date, "10:30" for time, "2026-01" for month), so neither
// the browser nor the agent does date arithmetic.
struct DateTimePopupParams {
  string input_type;   // "date"|"time"|"datetime-local"|"month"|"week"
  string value;        // current value (ISO), may be empty
  string min;          // ISO, may be empty
  string max;          // ISO, may be empty
  double step;
  array<DateTimeSuggestion> suggestions;
  gfx.mojom.Rect bounds;
};

// Browser -> renderer. The browser (ABP interceptor) holds a remote and calls
// exactly one of these to resolve a pending date/time picker.
interface DateTimePopupClient {
  // |value| is an ISO string; the renderer applies it via the input element.
  DidChooseValue(string value);
  DidCancel();
};
```

- [ ] **Step 2: Add to the mojom BUILD.gn**

In `third_party/blink/public/mojom/BUILD.gn`, find the `sources` entry `"choosers/date_time_chooser.mojom"` and add directly after it:
```gn
    "choosers/date_time_popup.mojom",
```

- [ ] **Step 3: Add the LocalFrameHost method**

In `third_party/blink/public/mojom/frame/frame.mojom`, immediately after the `ShowPopupMenu(...)` method (ends line ~551), add:
```mojom
  // ABP: request the embedder's popup interceptor to handle a date/time
  // input's picker. Mirrors ShowPopupMenu. If no interceptor is present the
  // browser calls DidCancel() on |client|.
  ShowDateTimePopup(DateTimePopupParams params,
                    pending_remote<DateTimePopupClient> client);
```
At the top of `frame.mojom` with the other imports, add:
```mojom
import "third_party/blink/public/mojom/choosers/date_time_popup.mojom";
```

- [ ] **Step 4: Compile the mojom target**

Run: `cd /home/paladin/src/src && autoninja -C out/Default blink_mojom_blink__generator`
Expected: build succeeds (generates `date_time_popup.mojom-blink.h`, etc.).

- [ ] **Step 5: Commit**
```bash
git add third_party/blink/public/mojom/choosers/date_time_popup.mojom \
        third_party/blink/public/mojom/BUILD.gn \
        third_party/blink/public/mojom/frame/frame.mojom
git commit -m "mojom: add ShowDateTimePopup + DateTimePopupClient for ABP"
```

### Task B2: Extend `content::PopupInterceptor`

**Files:**
- Modify: `content/public/browser/popup_interceptor.h`

- [ ] **Step 1: Add the method + includes**

Add include near the top (after the popup_menu forward include):
```cpp
#include "third_party/blink/public/mojom/choosers/date_time_popup.mojom-forward.h"
```
Inside `class PopupInterceptor`, after `OnSelectPopupRequested(...)`:
```cpp
  // Called when a date/time <input> requests its picker. Returns true if
  // intercepted (native picker suppressed). The interceptor takes ownership of
  // |client| to deliver the chosen ISO value (or cancel).
  virtual bool OnDateTimePopupRequested(
      RenderFrameHost* rfh,
      mojo::PendingRemote<blink::mojom::DateTimePopupClient> client,
      blink::mojom::DateTimePopupParamsPtr params) = 0;
```

- [ ] **Step 2: Compile content_public** (will fail until B5 implements the override — expected at this point; this task is the interface only)

Run: `cd /home/paladin/src/src && autoninja -C out/Default content/public/browser` 
Expected: compiles the header (no .cc). Full link deferred until B5.

- [ ] **Step 3: Commit**
```bash
git add content/public/browser/popup_interceptor.h
git commit -m "content: add PopupInterceptor::OnDateTimePopupRequested"
```

### Task B3: Browser-side `ShowDateTimePopup` → interceptor

**Files:**
- Modify: `content/browser/renderer_host/render_frame_host_impl.h` (declare override, near `ShowPopupMenu`)
- Modify: `content/browser/renderer_host/render_frame_host_impl.cc` (implement, after `ShowPopupMenu` at line 9377)

- [ ] **Step 1: Declare the override** in `render_frame_host_impl.h` next to the `ShowPopupMenu` declaration:
```cpp
  void ShowDateTimePopup(
      blink::mojom::DateTimePopupParamsPtr params,
      mojo::PendingRemote<blink::mojom::DateTimePopupClient> client) override;
```

- [ ] **Step 2: Implement** in `render_frame_host_impl.cc` directly after `ShowPopupMenu`'s closing brace (line 9377). Note: NOT gated by `USE_EXTERNAL_POPUP_MENU` (date/time has no native browser fallback):
```cpp
void RenderFrameHostImpl::ShowDateTimePopup(
    blink::mojom::DateTimePopupParamsPtr params,
    mojo::PendingRemote<blink::mojom::DateTimePopupClient> client) {
  if (auto* wc = WebContents::FromRenderFrameHost(this)) {
    if (auto* interceptor = wc->GetPopupInterceptor()) {
      if (interceptor->OnDateTimePopupRequested(this, std::move(client),
                                                std::move(params))) {
        return;  // Intercepted.
      }
    }
  }
  // No interceptor: tell the renderer to cancel so the field stays editable.
  mojo::Remote<blink::mojom::DateTimePopupClient> bound(std::move(client));
  bound->DidCancel();
}
```
Add include near the other popup includes:
```cpp
#include "third_party/blink/public/mojom/choosers/date_time_popup.mojom.h"
```

- [ ] **Step 3: Compile** `autoninja -C out/Default content` — expected: fails to link only on the pure-virtual override in any other PopupInterceptor impl; the `content` browser compiles. (AbpPopupInterceptor override lands in B5.)

- [ ] **Step 4: Commit**
```bash
git add content/browser/renderer_host/render_frame_host_impl.h \
        content/browser/renderer_host/render_frame_host_impl.cc
git commit -m "content: route ShowDateTimePopup to the PopupInterceptor"
```

### Task B4: Renderer — `AbpDateTimeChooser` + hook

**Files:**
- Create: `third_party/blink/renderer/core/html/forms/abp_date_time_chooser.{h,cc}`
- Modify: `third_party/blink/renderer/core/html/forms/build/forms.gni` (or `core/BUILD.gn` forms sources) — add the new files
- Modify: `third_party/blink/renderer/core/page/chrome_client_impl.cc:809-817` (hook `OpenDateTimeChooser`)

**Model `external_date_time_chooser.{h,cc}` exactly.** Differences: it implements
`blink::mojom::blink::DateTimePopupClient` (a `mojo::Receiver`), sends
`GetLocalFrameHostRemote().ShowDateTimePopup(params, receiver.BindNewPipeAndPassRemote())`,
and on `DidChooseValue(const String& iso)` calls `client_->DidChooseValue(iso)`
(the **String** overload — `date_time_chooser_client.h:49`); on `DidCancel()`
calls `client_->DidEndChooser()`.

- [ ] **Step 1: Create `abp_date_time_chooser.h`** (mirror `external_date_time_chooser.h`):
```cpp
// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_FORMS_ABP_DATE_TIME_CHOOSER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_FORMS_ABP_DATE_TIME_CHOOSER_H_

#include "third_party/blink/public/mojom/choosers/date_time_popup.mojom-blink.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/html/forms/date_time_chooser.h"
#include "third_party/blink/renderer/platform/mojo/heap_mojo_receiver.h"

namespace blink {
class DateTimeChooserClient;
class LocalFrame;

// ABP: routes a date/time input's picker to the browser-side popup
// interceptor instead of opening the in-renderer page popup.
class CORE_EXPORT AbpDateTimeChooser final
    : public DateTimeChooser,
      public mojom::blink::DateTimePopupClient {
 public:
  AbpDateTimeChooser(LocalFrame*, DateTimeChooserClient*,
                     const DateTimeChooserParameters&);
  ~AbpDateTimeChooser() override;
  void Trace(Visitor*) const override;

  // DateTimeChooser:
  void EndChooser() override;
  AXObject* RootAXObject(Element* popup_owner) override;
  bool IsPickerVisible() const override;

  // mojom::blink::DateTimePopupClient:
  void DidChooseValue(const String& value) override;
  void DidCancel() override;

 private:
  Member<DateTimeChooserClient> client_;
  HeapMojoReceiver<mojom::blink::DateTimePopupClient, AbpDateTimeChooser>
      receiver_;
  bool is_visible_ = false;
};
}  // namespace blink
#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_FORMS_ABP_DATE_TIME_CHOOSER_H_
```

- [ ] **Step 2: Create `abp_date_time_chooser.cc`.** Constructor builds `DateTimePopupParams`, formatting the value/min/max to ISO via the input type serializer. Use `InputType::Serialize` from the type carried in `DateTimeChooserParameters::type`; obtain the type string via `InputType::TypeToString(params.type)`. The current/min/max strings come from `params` — `DateTimeChooserParameters` exposes `double_value`, `minimum`, `maximum`; serialize each with the type's `SerializeWithComponents`/`Serialize` (verify the exact helper against `base_temporal_input_type.cc::SetupDateTimeChooserParameters`, which builds these doubles — reuse its inverse). Then:
```cpp
#include "third_party/blink/renderer/core/html/forms/abp_date_time_chooser.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/forms/date_time_chooser_client.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"

namespace blink {

AbpDateTimeChooser::AbpDateTimeChooser(
    LocalFrame* frame,
    DateTimeChooserClient* client,
    const DateTimeChooserParameters& params)
    : client_(client), receiver_(this, frame->DomWindow()) {
  auto popup = mojom::blink::DateTimePopupParams::New();
  popup->input_type = InputType::TypeToString(params.type);
  // popup->value/min/max := ISO serialization of params.double_value/min/max
  //   (see SetupDateTimeChooserParameters for the matching doubles).
  popup->step = params.step;
  // popup->suggestions := map params.suggestions -> DateTimeSuggestion
  // popup->bounds := params.anchor_rect_in_screen (gfx::Rect)
  is_visible_ = true;
  frame->GetLocalFrameHostRemote().ShowDateTimePopup(
      std::move(popup), receiver_.BindNewPipeAndPassRemote(
                            frame->GetTaskRunner(TaskType::kUserInteraction)));
}

AbpDateTimeChooser::~AbpDateTimeChooser() = default;

void AbpDateTimeChooser::DidChooseValue(const String& value) {
  is_visible_ = false;
  if (client_)
    client_->DidChooseValue(value);
  if (client_)
    client_->DidEndChooser();
}

void AbpDateTimeChooser::DidCancel() {
  is_visible_ = false;
  if (client_)
    client_->DidEndChooser();
}

void AbpDateTimeChooser::EndChooser() {
  receiver_.reset();
  is_visible_ = false;
  if (client_)
    client_->DidEndChooser();
}

AXObject* AbpDateTimeChooser::RootAXObject(Element*) { return nullptr; }
bool AbpDateTimeChooser::IsPickerVisible() const { return is_visible_; }

void AbpDateTimeChooser::Trace(Visitor* visitor) const {
  visitor->Trace(client_);
  visitor->Trace(receiver_);
  DateTimeChooser::Trace(visitor);
}
}  // namespace blink
```
> Note: the ISO serialization of `value/min/max/suggestions` and the exact bounds field are the one discovery item — confirm the serializer in `base_temporal_input_type.cc` (the file that *builds* `DateTimeChooserParameters`) and invert it. Add `#include` for `input_type.h`.

- [ ] **Step 3: Register the source** in the forms sources list (search `external_date_time_chooser.cc` in `third_party/blink/renderer/core/BUILD.gn` and add `abp_date_time_chooser.{cc,h}` beside it).

- [ ] **Step 4: Hook `OpenDateTimeChooser`** in `chrome_client_impl.cc` (line 809). Replace the body's chooser selection with an ABP-first branch:
```cpp
DateTimeChooser* ChromeClientImpl::OpenDateTimeChooser(
    LocalFrame* frame,
    DateTimeChooserClient* picker_client,
    const DateTimeChooserParameters& parameters) {
  NotifyPopupOpeningObservers();
  // ABP fork: always route date/time pickers to the browser interceptor.
  return MakeGarbageCollected<AbpDateTimeChooser>(frame, picker_client,
                                                  parameters);
}
```
Add `#include "third_party/blink/renderer/core/html/forms/abp_date_time_chooser.h"`.

- [ ] **Step 5: Compile Blink** `autoninja -C out/Default blink_core` — expected: success.

- [ ] **Step 6: Commit**
```bash
git add third_party/blink/renderer/core/html/forms/abp_date_time_chooser.h \
        third_party/blink/renderer/core/html/forms/abp_date_time_chooser.cc \
        third_party/blink/renderer/core/BUILD.gn \
        third_party/blink/renderer/core/page/chrome_client_impl.cc
git commit -m "blink: route date/time pickers to AbpDateTimeChooser"
```

### Task B5: `AbpPopupInterceptor` date/time support

**Files:**
- Modify: `chrome/browser/abp/abp_popup_interceptor.h`
- Modify: `chrome/browser/abp/abp_popup_interceptor.cc`

Mirror the select members exactly (`OnSelectPopupRequested` → `OnDateTimePopupRequested`, etc.).

- [ ] **Step 1: Header** — add struct + members:
```cpp
#include "third_party/blink/public/mojom/choosers/date_time_popup.mojom.h"

struct PendingDateTimePopup {
  PendingDateTimePopup();
  ~PendingDateTimePopup();
  PendingDateTimePopup(PendingDateTimePopup&&);
  PendingDateTimePopup& operator=(PendingDateTimePopup&&);
  std::string tab_id;
  mojo::Remote<blink::mojom::DateTimePopupClient> client;
  blink::mojom::DateTimePopupParamsPtr params;
};
```
In `AbpPopupInterceptor`, add the override + methods (next to the select ones):
```cpp
  bool OnDateTimePopupRequested(
      content::RenderFrameHost* rfh,
      mojo::PendingRemote<blink::mojom::DateTimePopupClient> client,
      blink::mojom::DateTimePopupParamsPtr params) override;
  bool RespondToDateTimePopup(const std::string& popup_id,
                              const std::string& iso_value);
  bool CancelDateTimePopup(const std::string& popup_id);
  base::Value::Dict GetPendingDateTimePopup(const std::string& popup_id) const;
```
And a private map + id counter:
```cpp
  std::map<std::string, PendingDateTimePopup> pending_datetime_popups_;
  int next_datetime_popup_id_ = 1;
  std::string GenerateDateTimePopupId();
```

- [ ] **Step 2: Implementation** in `abp_popup_interceptor.cc` (mirror `OnSelectPopupRequested` at line 30):
```cpp
bool AbpPopupInterceptor::OnDateTimePopupRequested(
    content::RenderFrameHost* rfh,
    mojo::PendingRemote<blink::mojom::DateTimePopupClient> client,
    blink::mojom::DateTimePopupParamsPtr params) {
  if (!controller_)
    return false;
  std::string tab_id;
  if (rfh) {
    if (auto* wc = content::WebContents::FromRenderFrameHost(rfh))
      tab_id = controller_->GetTabIdForWebContents(wc);
  }
  if (tab_id.empty())
    return false;

  std::string popup_id = GenerateDateTimePopupId();

  base::Value::Dict event_data;
  event_data.Set("type", "datetime_picker_open");
  event_data.Set("id", popup_id);
  event_data.Set("tab_id", tab_id);
  event_data.Set("input_type", params->input_type);
  event_data.Set("value", params->value);
  event_data.Set("min", params->min);
  event_data.Set("max", params->max);
  event_data.Set("step", params->step);
  base::Value::Dict bounds;
  bounds.Set("x", params->bounds.x());
  bounds.Set("y", params->bounds.y());
  bounds.Set("width", params->bounds.width());
  bounds.Set("height", params->bounds.height());
  event_data.Set("bounds", std::move(bounds));
  // (suggestions: map params->suggestions to a list if non-empty.)

  PendingDateTimePopup pending;
  pending.tab_id = tab_id;
  pending.client.Bind(std::move(client));
  pending.params = std::move(params);
  pending.client.set_disconnect_handler(base::BindOnce(
      [](AbpPopupInterceptor* self, std::string id) {
        self->pending_datetime_popups_.erase(id);
      },
      base::Unretained(this), popup_id));
  pending_datetime_popups_[popup_id] = std::move(pending);

  controller_->EmitPopupEvent("datetime_picker_open", std::move(event_data));
  return true;
}

bool AbpPopupInterceptor::RespondToDateTimePopup(
    const std::string& popup_id, const std::string& iso_value) {
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
  return result;
}

std::string AbpPopupInterceptor::GenerateDateTimePopupId() {
  return "dtpopup_" + base::NumberToString(next_datetime_popup_id_++);
}
```
Define the `PendingDateTimePopup` ctor/dtor/move members at the top of the .cc (mirror `PendingSelectPopup`'s out-of-line defs).

- [ ] **Step 3: Extend `CleanupForTab`** to also erase date/time popups for the tab (add the same erase loop over `pending_datetime_popups_`).

- [ ] **Step 4: Compile** `autoninja -C out/Default chrome` — expected: success (the override now satisfies the pure virtual from B2/B3).

- [ ] **Step 5: Commit**
```bash
git add chrome/browser/abp/abp_popup_interceptor.h chrome/browser/abp/abp_popup_interceptor.cc
git commit -m "abp: handle date/time picker interception in AbpPopupInterceptor"
```

### Task B6: REST route + handler

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h` (declare `HandleDateTimePopup`, next to `HandleSelectPopup` at line 602)
- Modify: `chrome/browser/abp/abp_controller.cc` (route + handler)

- [ ] **Step 1: Route** — near the select route (`abp_controller.cc:2422-2425`), add a `datetime-picker` branch:
```cpp
    if (resource == "datetime-picker") {
      const std::string& popup_id = segments[3];
      if (method == "POST") {
        HandleDateTimePopup(popup_id, params, std::move(callback));
        return;
      }
    }
```

- [ ] **Step 2: Handler** — mirror `HandleSelectPopup`:
```cpp
void AbpController::HandleDateTimePopup(const std::string& popup_id,
                                        const base::Value::Dict& params,
                                        ResponseCallback callback) {
  if (!popup_interceptor_) {
    SendError(500, "Popup interceptor not initialized", std::move(callback));
    return;
  }
  if (params.FindBool("cancel").value_or(false)) {
    if (popup_interceptor_->CancelDateTimePopup(popup_id)) {
      base::Value::Dict result;
      result.Set("success", true);
      result.Set("cancelled", true);
      SendJson(200, base::Value(std::move(result)), std::move(callback));
    } else {
      SendError(404, "Date/time popup not found: " + popup_id,
                std::move(callback));
    }
    return;
  }
  const std::string* value = params.FindString("value");
  if (!value) {
    SendError(400, "Missing 'value' (ISO string)", std::move(callback));
    return;
  }
  auto info = popup_interceptor_->GetPendingDateTimePopup(popup_id);
  if (info.empty()) {
    SendError(404, "Date/time popup not found: " + popup_id,
              std::move(callback));
    return;
  }
  if (!popup_interceptor_->RespondToDateTimePopup(popup_id, *value)) {
    SendError(500, "Failed to respond to date/time popup", std::move(callback));
    return;
  }
  base::Value::Dict result;
  result.Set("success", true);
  result.Set("tab_id", *info.FindString("tab_id"));
  SendJson(200, base::Value(std::move(result)), std::move(callback));
}
```

- [ ] **Step 3: Compile** `autoninja -C out/Default chrome` — expected: success.

- [ ] **Step 4: Commit**
```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "abp: add POST /api/v1/datetime-picker/{id} handler"
```

### Task B7: MCP tool `browser_datetime_picker`

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc` (register tool + dispatch, mirroring the select-picker tool)
- Modify: `chrome/browser/abp/abp_tool_builder.cc` if the select tool's schema lives there

- [ ] **Step 1:** Find the existing select-picker MCP tool (`grep -n "select_picker\|browser_select" chrome/browser/abp/abp_mcp_handler.cc`) and add a sibling `browser_datetime_picker` tool with args `{ id: string, value?: string, cancel?: bool }` whose handler builds `{"value": ...}` or `{"cancel": true}` and calls the same path as `POST /api/v1/datetime-picker/{id}` (reuse the controller route or call `HandleDateTimePopup` via the controller, exactly as the select tool does for select).

- [ ] **Step 2: Compile** `autoninja -C out/Default chrome` — expected: success.

- [ ] **Step 3: Commit**
```bash
git add chrome/browser/abp/abp_mcp_handler.cc chrome/browser/abp/abp_tool_builder.cc
git commit -m "abp: add browser_datetime_picker MCP tool"
```

### Task B8: Full build, launch test, docs

**Files:**
- Use: `chrome/browser/abp/test_pages/datetime-picker-test.html` (already added)
- Create: `chrome/browser/abp/test_pages/run_datetime_picker_test.sh`
- Create: `chrome/browser/abp/test_pages/datetime_picker_check.py`
- Modify: `plans/API.md`, `plans/mcp.md`

- [ ] **Step 1: Full build**

Run: `cd /home/paladin/src/src && autoninja -C out/Default chrome`
Expected: `Build Succeeded`; confirm `out/Default/abp` mtime updated and contains the new strings:
```bash
grep -lac "datetime_picker_open" out/Default/abp   # expect 1
```

- [ ] **Step 2: Write the checker** `datetime_picker_check.py` (model `zoom_click_check.py`). For each of the five inputs on the page: get the input's center via `__rects()`, `POST /click`, read the click envelope's `events` and assert one has `type=="datetime_picker_open"` with the matching `input_type` and a non-null `value`/`min`/`max`. Then `POST /api/v1/datetime-picker/{id}` with a new ISO `value`, and assert via `/execute` that the input's `.value` changed and `window.__log` recorded an `input`+`change`. Also assert a `<select>` on the page now yields `select_open` (Part A regression).

- [ ] **Step 3: Write the runner** `run_datetime_picker_test.sh` (copy `run_zoom_click_test.sh`; launch `out/Default/abp` with `--abp-port`, `--disable-gpu`, the datetime test page; invoke the checker; clean up by PID).

- [ ] **Step 4: Run it**

Run: `bash chrome/browser/abp/test_pages/run_datetime_picker_test.sh 8355`
Expected: `PASS` for all five date/time types AND the `select_open` regression.

- [ ] **Step 5: Document** — add the `POST /api/v1/datetime-picker/{id}` row to `plans/API.md` and the `browser_datetime_picker` tool to `plans/mcp.md`; note in CLAUDE.md's feature list that date/time pickers are intercepted and `<select>` now works on Linux/Windows.

- [ ] **Step 6: Commit**
```bash
git add chrome/browser/abp/test_pages/run_datetime_picker_test.sh \
        chrome/browser/abp/test_pages/datetime_picker_check.py \
        chrome/browser/abp/test_pages/datetime-picker-test.html \
        plans/API.md plans/mcp.md CLAUDE.md
git commit -m "abp: launch test + docs for date/time picker interception"
```

---

## Verification checklist (run after Task B8)

- [ ] `out/Default/abp` rebuilt, contains `datetime_picker_open`.
- [ ] Launch test: all five `<input>` types emit `datetime_picker_open` with correct `input_type`; responding sets the value (`input`+`change` fire).
- [ ] Launch test: `<select>` emits `select_open` on Linux (Part A).
- [ ] No date math in ABP/C++ (values are ISO strings end-to-end).
