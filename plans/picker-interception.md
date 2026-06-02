# Picker Interception: Cross-platform `<select>` + Date/Time

Status: design approved 2026-06-01

## Problem

ABP intercepts native popups so an AI agent can see the choices and respond via
the REST/MCP API. Today this only works for `<select>` on macOS/Android. Two
gaps:

1. **`<select>` on Linux/Windows.** The select interceptor
   (`AbpPopupInterceptor::OnSelectPopupRequested`) is wired into
   `RenderFrameHostImpl::ShowPopupMenu`, which is compiled only when
   `USE_EXTERNAL_POPUP_MENU` is true — i.e. macOS/Android
   (`content/public/common/features.gni`). On Linux/Windows, Blink renders
   `<select>` popups as internal `WebPagePopup`s that never reach that hook, so
   the agent gets an empty `events` array and no choices.

2. **Date/time pickers on every desktop platform.** `<input type=date|time|
   datetime-local|month|week>` use `ChromeClientImpl::OpenDateTimeChooser` →
   `DateTimeChooserImpl` → `OpenPagePopup` (a `WebPagePopup`) on all desktop
   platforms, including macOS. ABP has no interception for them. Empirically
   (experiment 2026-06-01): clicking a date input yields `events: []`, the
   popup is not in the viewport screenshot (it is a separate child widget), and
   a center-click lands on a sub-segment rather than opening the picker. The
   agent is blind.

Both pickers are also **unscreenshottable**: ABP's screenshot is a
`GrabViewSnapshot` of the main `RenderWidgetHostView`; child popup widgets are
never captured. So the fix must surface picker contents as **structured data**,
not pixels.

## Scope

In scope:
- **Part A:** Make the existing `<select>` interceptor work on Linux + Windows.
- **Part B:** Intercept the date/time family (`date`, `time`, `datetime-local`,
  `month`, `week`), emit a `datetime_picker_open` event, and provide a respond
  action.

Out of scope (separate mechanisms, future work):
- `<input type=color>` (`OpenColorChooser` / `WebContentsDelegate::OpenColorChooser`).
- `<input list>` datalist suggestions.

## Part A — `<select>` on Linux/Windows

One-line build change: enable external popup menus on desktop Linux/Windows so
Blink routes `<select>` through `ShowPopupMenu` into the existing interceptor,
exactly as on macOS.

`content/public/common/features.gni`:
```
use_external_popup_menu = is_android || (is_apple && use_blink) ||
                          ((is_linux || is_win) && use_blink)
```

Rationale and risk (verified):
- `ExternalPopupMenu` is already referenced unconditionally in
  `ChromeClientImpl::OpenPopupMenu`; the flag only flips the default of
  `use_external_popup_menus_`. So Blink already compiles it on Linux.
- Only 16 `USE_EXTERNAL_POPUP_MENU` sites exist; the `actor_task` ones are
  additionally `IS_MAC`-gated, so unaffected. `WebContentsViewAura::ShowPopupMenu`
  is `NOTIMPLEMENTED()` but is bypassed because `AbpPopupInterceptor` always
  returns `true` for ABP tabs.
- Behavioral change: `<select>` popups on Linux/Windows become ABP-intercepted
  (no native dropdown in human input mode) — identical to current macOS
  behavior. Accepted as consistent.

No ABP code changes for Part A; it reuses `OnSelectPopupRequested` as-is.

## Part B — Date/time picker interception

Model it on `ExternalDateTimeChooser` (the existing async, mojo-based chooser
used on Android), routed to ABP, **without** disabling `InputMultipleFieldsUI`
(that runtime feature controls the segmented MM/DD/YYYY field rendering, which
must stay on).

### Data flow

1. **Renderer hook** — in `ChromeClientImpl::OpenDateTimeChooser(frame, client,
   params)` (single entry for all five types). In the ABP fork build this
   **always** creates an `AbpDateTimeChooser` (modeled on
   `ExternalDateTimeChooser`) instead of `DateTimeChooserImpl`; it sends the
   params to the browser and suppresses the page popup. The browser decides:
   if the WebContents has a `PopupInterceptor`, it intercepts (the normal ABP
   path); if not (e.g. a non-ABP WebContents), it responds with **cancel** and
   no popup opens — the field stays editable via native typing. We do not fall
   back to `DateTimeChooserImpl`, because that page popup is renderer-side and
   cannot be shown from the browser the way `view->ShowPopupMenu` shows the
   native select menu. (Upstream, non-ABP behavior is unchanged: this code is
   fork-local.)
2. **New mojo** — `LocalFrameHost.ShowDateTimePopup(DateTimePopupParams,
   pending_remote<blink.mojom.DateTimeChooserClient>)`, parallel to
   `ShowPopupMenu`. Params carry: `input_type`, current `value` (ISO string),
   `min`, `max`, `step`, `suggestions[]`, anchor `bounds`.
3. **Browser hook** — `RenderFrameHostImpl::ShowDateTimePopup` →
   new `content::PopupInterceptor::OnDateTimePopupRequested(rfh, client, params)`,
   mirroring `OnSelectPopupRequested`.
4. **ABP** — `AbpPopupInterceptor` gains, in parallel with its select members:
   - a `pending_datetime_popups_` map + `PendingDateTimePopup` struct,
   - `OnDateTimePopupRequested(...)` — store pending, emit event, return `true`,
   - `RespondToDateTimePopup(id, iso_value)` / `CancelDateTimePopup(id)`,
   - `GetPendingDateTimePopup(id)`,
   - `CleanupForTab` extended to clear date/time popups too.
   Event emitted via the existing `AbpController::EmitPopupEvent`.

### Event (in the action envelope)

```json
{ "type": "datetime_picker_open", "id": "<popup_id>", "tab_id": "...",
  "input_type": "date",          // date|time|datetime-local|month|week
  "value": "2026-01-15",          // current value, ISO (per input_type)
  "min": "2020-01-01", "max": "2030-12-31", "step": 1,
  "suggestions": [ { "value": "...", "label": "..." } ],
  "bounds": { "x": 0, "y": 0, "width": 0, "height": 0 } }
```
One unified event type with an `input_type` field (not five event names).

### Action (REST + MCP)

Mirrors `/api/v1/select/{id}`:
- `POST /api/v1/datetime-picker/{id}` — body `{ "value": "2026-06-15" }`
  (ISO per `input_type`) **or** `{ "cancel": true }`.
- New MCP tool `browser_datetime_picker`.
- Routing added in `AbpController` next to the existing `select` route; new
  handler `HandleDateTimePopup` paralleling `HandleSelectPopup`.

### Value conversion

The agent speaks **ISO strings**. The response mojo carries the string to the
renderer, and **Blink applies it** via the input element's own value setter
(Blink owns the per-type parsers). ABP performs no date arithmetic. Invalid /
out-of-range values are rejected by Blink's parser; ABP returns a 400 if the
renderer reports failure.

### Suppression / input mode

Intercepts unconditionally for ABP tabs, identical to the select interceptor
(so date/time human-mode behavior matches select today). Documented as a known,
consistent limitation; revisiting input-mode-aware interception is future work.

## Testing

Launch test (pattern: `test_pages/run_zoom_click_test.sh`):
- `test_pages/datetime-picker-test.html` (already added) with all five input
  types.
- Driver: for each type, click the input to trigger the picker; assert the
  click action envelope contains a `datetime_picker_open` event with the
  correct `input_type`, `value`, `min`, `max`. Then `POST
  /datetime-picker/{id}` with a new ISO value and assert the input's `value`
  changed and `input`/`change` fired (`__log`).
- Part A regression: extend the page with a `<select>` and assert
  `select_open` now appears on Linux.

Requires a full Blink+content rebuild (both parts touch `content`/Blink).

## Files (anticipated)

- `content/public/common/features.gni` — Part A flag (done, pending).
- `content/public/browser/popup_interceptor.h` — add `OnDateTimePopupRequested`.
- `content/browser/renderer_host/render_frame_host_impl.{h,cc}` — new
  `ShowDateTimePopup` mojo impl → interceptor.
- `third_party/blink/public/mojom/...local_frame_host.mojom` — new
  `ShowDateTimePopup` method + `DateTimePopupParams`.
- `third_party/blink/renderer/core/page/chrome_client_impl.cc` +
  `core/html/forms/abp_date_time_chooser.{h,cc}` (new) — renderer hook/chooser.
- `chrome/browser/abp/abp_popup_interceptor.{h,cc}` — date/time pending +
  methods.
- `chrome/browser/abp/abp_controller.{h,cc}` — route + `HandleDateTimePopup`.
- `chrome/browser/abp/abp_mcp_handler.cc` + `abp_tool_builder` — MCP tool.
- `chrome/browser/abp/test_pages/` — test page + driver.
- `plans/API.md`, `plans/mcp.md` — document the new endpoint/tool.
