// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_input_dispatcher.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "chrome/browser/abp/abp_action_context.h"
#include "chrome/browser/abp/abp_controller.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/browser_thread.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/blink/public/common/input/synthetic_web_input_event_builders.h"
#include "ui/events/keycodes/dom/keycode_converter.h"

namespace abp {

namespace {

// Key timing constants for realistic input simulation.
constexpr int kKeyDwellMs = 20;      // Hold time between keyDown and keyUp
constexpr int kInterKeyDelayMs = 50; // Pause between consecutive key events

// Convert ABP modifier bitmask (1=Alt, 2=Ctrl, 4=Meta, 8=Shift) to
// blink::WebInputEvent modifier flags.
int ModifierFlagsToWebModifiers(int flags) {
  int result = 0;
  if (flags & 1)
    result |= blink::WebInputEvent::kAltKey;
  if (flags & 2)
    result |= blink::WebInputEvent::kControlKey;
  if (flags & 4)
    result |= blink::WebInputEvent::kMetaKey;
  if (flags & 8)
    result |= blink::WebInputEvent::kShiftKey;
  return result;
}

// Returns the effective page zoom factor for `wc` (1.0 == 100%).
//
// Agent-facing coordinates (the ones an agent reads off a screenshot) are in
// viewport DIP pixels. CDP Input.dispatchMouseEvent, however, interprets its
// x/y as CSS pixels and multiplies them by this same zoom factor internally
// (see content's InputHandler::ScaleFactor). Dividing a DIP coordinate by the
// zoom factor before handing it to CDP therefore lands the event on the exact
// pixel the agent saw. At 100% zoom this is a no-op; at ABP's default 80% zoom
// it removes a ~25% positional error. Returns 1.0 for a null/!ready contents.
double AbpPageZoomFactor(content::WebContents* wc) {
  if (!wc)
    return 1.0;
  double factor =
      blink::ZoomLevelToZoomFactor(content::HostZoomMap::GetZoomLevel(wc));
  return factor > 0.0 ? factor : 1.0;
}

// US keyboard layout mapping for symbols/punctuation.
// Maps a character to its physical key's DOM code string, Windows virtual key
// code, and whether Shift is required to produce it.
struct UsKeyMapping {
  const char* code;          // DOM physical key code (e.g. "Digit4")
  int windows_virtual_key;   // VK code of the physical key
  bool shift;                // Whether Shift is required
};

// Returns the US keyboard mapping for a symbol/punctuation character,
// or nullptr if not found.
const UsKeyMapping* GetUsKeyMapping(char c) {
  // clang-format off
  static constexpr struct { char ch; UsKeyMapping mapping; } kTable[] = {
      // Shifted digit row: Shift + Digit → symbol
      {'!', {"Digit1",       '1',  true}},
      {'@', {"Digit2",       '2',  true}},
      {'#', {"Digit3",       '3',  true}},
      {'$', {"Digit4",       '4',  true}},
      {'%', {"Digit5",       '5',  true}},
      {'^', {"Digit6",       '6',  true}},
      {'&', {"Digit7",       '7',  true}},
      {'*', {"Digit8",       '8',  true}},
      {'(', {"Digit9",       '9',  true}},
      {')', {"Digit0",       '0',  true}},

      // OEM keys: unshifted
      {'`', {"Backquote",    0xC0, false}},
      {'-', {"Minus",        0xBD, false}},
      {'=', {"Equal",        0xBB, false}},
      {'[', {"BracketLeft",  0xDB, false}},
      {']', {"BracketRight", 0xDD, false}},
      {'\\',{"Backslash",    0xDC, false}},
      {';', {"Semicolon",    0xBA, false}},
      {'\'',{"Quote",        0xDE, false}},
      {',', {"Comma",        0xBC, false}},
      {'.', {"Period",       0xBE, false}},
      {'/', {"Slash",        0xBF, false}},

      // OEM keys: shifted
      {'~', {"Backquote",    0xC0, true}},
      {'_', {"Minus",        0xBD, true}},
      {'+', {"Equal",        0xBB, true}},
      {'{', {"BracketLeft",  0xDB, true}},
      {'}', {"BracketRight", 0xDD, true}},
      {'|', {"Backslash",    0xDC, true}},
      {':', {"Semicolon",    0xBA, true}},
      {'"', {"Quote",        0xDE, true}},
      {'<', {"Comma",        0xBC, true}},
      {'>', {"Period",       0xBE, true}},
      {'?', {"Slash",        0xBF, true}},
  };
  // clang-format on

  for (const auto& entry : kTable) {
    if (entry.ch == c)
      return &entry.mapping;
  }
  return nullptr;
}

}  // namespace

AbpInputDispatcher::AbpInputDispatcher(AbpController* controller)
    : controller_(controller) {}

AbpInputDispatcher::~AbpInputDispatcher() = default;

void AbpInputDispatcher::ForwardKeyEvent(content::WebContents* wc,
                                         blink::WebInputEvent::Type type,
                                         const KeyInfo& info,
                                         int web_modifiers) {
  // Mark as debugger-originated so ABP's system input filter in
  // RenderInputRouter allows the event through (same as CDP mouse events).
  web_modifiers |= blink::WebInputEvent::kFromDebugger;

  auto* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv)
    return;
  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      rwhv->GetRenderWidgetHost());
  if (!rwhi)
    return;

  // Get the focused widget (handles iframes, etc.)
  if (rwhi->delegate()) {
    auto* target = rwhi->delegate()->GetFocusedRenderWidgetHost(rwhi);
    if (target)
      rwhi = target;
  }

  if (type == blink::WebInputEvent::Type::kKeyDown) {
    // Send the full three-event sequence that real keyboard input produces:
    //   1. kRawKeyDown → DOM "keydown"
    //   2. kChar → DOM "keypress" (only for keys that produce text)
    //   3. (kKeyUp sent separately by caller)
    // This matches macOS's native event sequence and is more reliable than
    // relying on blink's kKeyDown→kChar fallthrough logic.

    // 1. Send kRawKeyDown (generates DOM "keydown")
    base::TimeTicks now = base::TimeTicks::Now();
    input::NativeWebKeyboardEvent raw_down(
        blink::WebInputEvent::Type::kRawKeyDown, web_modifiers, now);
    raw_down.windows_key_code = info.windows_virtual_key;
    raw_down.native_key_code = info.native_virtual_key;
    raw_down.dom_code = static_cast<int>(
        ui::KeycodeConverter::CodeStringToDomCode(info.code));
    raw_down.dom_key = static_cast<int>(
        ui::KeycodeConverter::KeyStringToDomKey(info.key));
    if (!info.text.empty()) {
      raw_down.text[0] = static_cast<char16_t>(info.text[0]);
      raw_down.unmodified_text[0] = raw_down.text[0];
    }
    raw_down.skip_if_unhandled = true;
    rwhi->ForwardKeyboardEvent(raw_down);

    // 2. Send kChar (generates DOM "keypress") — only if key produces text
    //    Use a distinct timestamp so the input pipeline doesn't coalesce it
    //    with the preceding kRawKeyDown.
    if (!info.text.empty()) {
      base::TimeTicks char_time = now + base::Microseconds(1);
      input::NativeWebKeyboardEvent char_event(
          blink::WebInputEvent::Type::kChar, web_modifiers, char_time);
      char_event.windows_key_code = info.windows_virtual_key;
      char_event.native_key_code = info.native_virtual_key;
      char_event.dom_code = static_cast<int>(
          ui::KeycodeConverter::CodeStringToDomCode(info.code));
      char_event.dom_key = static_cast<int>(
          ui::KeycodeConverter::KeyStringToDomKey(info.key));
      char_event.text[0] = static_cast<char16_t>(info.text[0]);
      char_event.unmodified_text[0] = char_event.text[0];
      char_event.skip_if_unhandled = true;
      rwhi->ForwardKeyboardEvent(char_event);
    }
  } else {
    // kKeyUp or other types — send as-is
    input::NativeWebKeyboardEvent event(type, web_modifiers,
                                        base::TimeTicks::Now());
    event.windows_key_code = info.windows_virtual_key;
    event.native_key_code = info.native_virtual_key;
    event.dom_code = static_cast<int>(
        ui::KeycodeConverter::CodeStringToDomCode(info.code));
    event.dom_key = static_cast<int>(
        ui::KeycodeConverter::KeyStringToDomKey(info.key));
    event.skip_if_unhandled = true;
    rwhi->ForwardKeyboardEvent(event);
  }
}

void AbpInputDispatcher::ForwardWheelEvent(content::WebContents* wc,
                                           double x,
                                           double y,
                                           double delta_x,
                                           double delta_y) {
  auto* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv)
    return;
  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      rwhv->GetRenderWidgetHost());
  if (!rwhi)
    return;

  // Get the focused widget (handles iframes, etc.)
  if (rwhi->delegate()) {
    auto* target = rwhi->delegate()->GetFocusedRenderWidgetHost(rwhi);
    if (target)
      rwhi = target;
  }

  float fx = static_cast<float>(x);
  float fy = static_cast<float>(y);
  // Negate deltas: ABP API convention (positive = down/right) is opposite
  // to WebMouseWheelEvent convention (positive = up/left).
  // InputRouterImpl::ScaleEvent multiplies deltas by device_scale_factor,
  // converting from DIP to physical pixels for the renderer. Since we provide
  // CSS pixel deltas (=DIP), the scaling gives the correct physical pixel
  // values. Do NOT pre-divide by DSF — that would halve the actual scroll.
  float fdx = -static_cast<float>(delta_x);
  float fdy = -static_cast<float>(delta_y);

  // Mark as kFromDebugger so ABP's input filter in RenderInputRouter allows
  // the event through (ABP blocks non-debugger input by default).
  int modifiers = blink::WebInputEvent::kFromDebugger;

  // macOS scroll handling requires the full gesture phase sequence:
  // kPhaseBegan -> kPhaseEnded. A lone kPhaseChanged is dropped.

  // 1. Send kPhaseBegan with the scroll deltas
  blink::WebMouseWheelEvent begin_event =
      blink::SyntheticWebMouseWheelEventBuilder::Build(
          fx, fy, fdx, fdy, modifiers,
          ui::ScrollGranularity::kScrollByPrecisePixel);
  begin_event.phase = blink::WebMouseWheelEvent::kPhaseBegan;
  rwhi->ForwardWheelEvent(begin_event);

  // 2. Send kPhaseEnded with zero deltas to close the gesture
  blink::WebMouseWheelEvent end_event =
      blink::SyntheticWebMouseWheelEventBuilder::Build(
          fx, fy, 0, 0, modifiers,
          ui::ScrollGranularity::kScrollByPrecisePixel);
  end_event.phase = blink::WebMouseWheelEvent::kPhaseEnded;
  rwhi->ForwardWheelEvent(end_event);
}

void AbpInputDispatcher::ForwardMouseMoveEvent(content::WebContents* wc,
                                               double x,
                                               double y) {
  auto* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv)
    return;
  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      rwhv->GetRenderWidgetHost());
  if (!rwhi)
    return;

  // Mark as kFromDebugger so ABP's input filter in RenderInputRouter allows
  // the event through (ABP blocks non-debugger input by default).
  int modifiers = blink::WebInputEvent::kFromDebugger;

  blink::WebMouseEvent move_event = blink::SyntheticWebMouseEventBuilder::Build(
      blink::WebInputEvent::Type::kMouseMove,
      static_cast<float>(x), static_cast<float>(y), modifiers);
  rwhi->ForwardMouseEvent(move_event);
}

void AbpInputDispatcher::Click(const std::string& tab_id,
                               const base::Value::Dict& params,
                               ResponseCallback callback) {
  // Validate params early
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    controller_->SendError(400, "Missing 'x' or 'y' parameter",
                           std::move(callback));
    return;
  }

  double click_x = *x_opt;
  double click_y = *y_opt;

  // Read optional button (default: "left")
  const std::string* button_param = params.FindString("button");
  std::string button = (button_param && (*button_param == "right" ||
                                          *button_param == "middle"))
                            ? *button_param
                            : "left";

  // Read optional clickCount (default: 1)
  int click_count = params.FindInt("click_count").value_or(1);
  if (click_count < 1) click_count = 1;
  if (click_count > 3) click_count = 3;

  // Read optional modifiers
  int mod_flags = 0;
  const base::Value::List* mod_list = params.FindList("modifiers");
  if (mod_list) {
    std::vector<std::string> modifiers;
    for (const auto& mod : *mod_list) {
      if (mod.is_string()) {
        modifiers.push_back(mod.GetString());
      }
    }
    mod_flags = ModifiersToFlags(modifiers);
  }

  // Use AbpActionContext for unified action flow:
  // Resume -> BeforeScreenshot -> Action -> Wait -> Pause -> AfterScreenshot -> Response
  AbpActionContext::RunWithOptions(
      controller_, tab_id, "click", params, controller_->GetDefaultActionOptions(),
      // Action callback - performs the actual click
      base::BindOnce(
          [](double coord_x, double coord_y, std::string btn, int count,
             int modifiers, AbpActionContext* ctx) {
            // Log cursor position before the action
            auto& tab_state = ctx->controller()->GetOrCreateTabState(ctx->tab_id());
            VLOG(1) << "ABP: Click action started - target=(" << coord_x << ", " << coord_y
                      << ") button=" << btn << " count=" << count
                      << " before_cursor=(" << tab_state.cursor.x << ", " << tab_state.cursor.y
                      << ") active=" << tab_state.cursor.active;

            // Update virtual cursor state via controller
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), coord_x,
                                                        coord_y);

            // Enable and set virtual cursor via Mojo for on-screen rendering
            content::WebContents* wc = ctx->web_contents();
            if (wc) {
              ctx->controller()->SetVirtualCursorEnabledViaMojo(wc, true);
              ctx->controller()->SetVirtualCursorViaMojo(wc, coord_x, coord_y,
                                                          true);
            } else {
              LOG(WARNING) << "ABP: Click action - WebContents not found for tab " << ctx->tab_id();
            }

            // Convert the agent's DIP coordinates to the CSS pixels CDP expects
            // (see AbpPageZoomFactor). The virtual cursor above intentionally
            // stays in DIP; only the dispatched mouse event is rescaled.
            double zoom_factor = AbpPageZoomFactor(wc);
            double cdp_x = coord_x / zoom_factor;
            double cdp_y = coord_y / zoom_factor;
            VLOG(1) << "ABP: Click zoom=" << zoom_factor << " DIP=(" << coord_x
                    << "," << coord_y << ") -> CSS=(" << cdp_x << "," << cdp_y
                    << ")";

            // Keep context alive through async fences + input dispatch.
            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            ctx->controller()->InsertVisualStateFence(
                ctx->tab_id(),
                base::BindOnce(
                    [](double x, double y, std::string button, int click_count,
                       int mods, scoped_refptr<AbpActionContext> action_ctx,
                       bool fence_ready) {
                      VLOG(1) << "ABP: Visual state fence "
                                << (fence_ready ? "ready" : "failed")
                                << " for click at (" << x << ", " << y << ")";

                      if (!fence_ready) {
                        action_ctx->OnActionError(
                            "VISUAL_STATE_ERROR",
                            "Failed to establish visual-state fence before click");
                        return;
                      }

                      AbpCdpClient* cdp_client = action_ctx->client();
                      if (!cdp_client) {
                        action_ctx->OnActionError("CDP_ERROR",
                                                  "CDP client lost");
                        return;
                      }

                      // Send mousePressed after cursor is confirmed visible.
                      base::Value::Dict press_params;
                      press_params.Set("type", "mousePressed");
                      press_params.Set("x", x);
                      press_params.Set("y", y);
                      press_params.Set("button", button);
                      press_params.Set("clickCount", click_count);
                      press_params.Set("modifiers", mods);

                      VLOG(1) << "ABP: Sending Input.dispatchMouseEvent (mousePressed) at ("
                                << x << ", " << y << ") button=" << button;

                      cdp_client->SendCommand(
                          "Input.dispatchMouseEvent", std::move(press_params),
                          base::BindOnce(
                              [](double x, double y, std::string button,
                                 int click_count, int mods,
                                 scoped_refptr<AbpActionContext> action_ctx,
                                 bool success, const std::string& result) {
                                if (!success) {
                                  LOG(ERROR) << "ABP: Input.dispatchMouseEvent (mousePressed) failed: " << result;
                                  action_ctx->OnActionError("CDP_ERROR", result);
                                  return;
                                }

                                VLOG(1) << "ABP: Input.dispatchMouseEvent (mousePressed) succeeded";

                                AbpCdpClient* cdp_client = action_ctx->client();
                                if (!cdp_client) {
                                  action_ctx->OnActionError(
                                      "CDP_ERROR", "CDP client lost");
                                  return;
                                }

                                // Send mouseReleased.
                                base::Value::Dict release_params;
                                release_params.Set("type", "mouseReleased");
                                release_params.Set("x", x);
                                release_params.Set("y", y);
                                release_params.Set("button", button);
                                release_params.Set("clickCount", click_count);
                                release_params.Set("modifiers", mods);

                                VLOG(1) << "ABP: Sending Input.dispatchMouseEvent (mouseReleased) at ("
                                          << x << ", " << y << ")";

                                cdp_client->SendCommand(
                                    "Input.dispatchMouseEvent",
                                    std::move(release_params),
                                    base::BindOnce(
                                        [](scoped_refptr<AbpActionContext> c,
                                           bool success,
                                           const std::string& result) {
                                          if (!success) {
                                            LOG(ERROR) << "ABP: Input.dispatchMouseEvent (mouseReleased) failed: " << result;
                                            c->OnActionError("CDP_ERROR",
                                                             result);
                                            return;
                                          }

                                          VLOG(1) << "ABP: Input.dispatchMouseEvent (mouseReleased) succeeded - click complete";

                                          base::Value::Dict res;
                                          res.Set("status", "clicked");
                                          c->SetResult(std::move(res));
                                          c->OnActionDispatched();
                                        },
                                        action_ctx));
                              },
                              x, y, button, click_count, mods, action_ctx));
                    },
                    cdp_x, cdp_y, std::move(btn), count, modifiers,
                    std::move(ctx_ref)));
          },
          click_x, click_y, std::move(button), click_count, mod_flags),
      std::move(callback));
}

void AbpInputDispatcher::Type(const std::string& tab_id,
                              const base::Value::Dict& params,
                              ResponseCallback callback) {
  // Validate params early
  const std::string* text = params.FindString("text");
  if (!text) {
    controller_->SendError(400, "Missing 'text' parameter", std::move(callback));
    return;
  }

  std::string text_copy = *text;

  // Use AbpActionContext for unified action flow.
  // Type uses native keyboard events — each character gets its own
  // keyDown + keyUp pair with a 2ms delay between characters, producing
  // the same DOM event chain as real typing (keydown → keypress → input → keyup).
  AbpActionContext::RunWithOptions(
      controller_, tab_id, "type", params, controller_->GetDefaultActionOptions(),
      base::BindOnce(
          [](std::string text_str, AbpInputDispatcher* dispatcher,
             AbpActionContext* ctx) {
            if (text_str.empty()) {
              base::Value::Dict res;
              res.Set("status", "typed");
              ctx->SetResult(std::move(res));
              ctx->OnActionDispatched();
              return;
            }
            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            dispatcher->TypeNextCharacter(ctx_ref, std::move(text_str), 0);
          },
          std::move(text_copy), this),
      std::move(callback));
}

void AbpInputDispatcher::TypeNextCharacter(
    scoped_refptr<AbpActionContext> ctx,
    std::string text,
    size_t char_index) {
  if (!ctx->web_contents() || char_index >= text.size()) {
    base::Value::Dict res;
    res.Set("status", "typed");
    ctx->SetResult(std::move(res));
    ctx->OnActionDispatched();
    return;
  }

  char c = text[char_index];
  int web_mods = 0;

  // Build KeyInfo for this character
  KeyInfo char_info;
  char_info.text = std::string(1, c);

  if (c >= 'a' && c <= 'z') {
    char_info.key = std::string(1, c);
    char_info.code = std::string("Key") + static_cast<char>(std::toupper(c));
    char_info.windows_virtual_key = std::toupper(c);
  } else if (c >= 'A' && c <= 'Z') {
    // Uppercase: send Shift + lowercase key
    char_info.key = std::string(1, c);
    char_info.code = std::string("Key") + c;
    char_info.windows_virtual_key = c;
    web_mods = blink::WebInputEvent::kShiftKey;
  } else if (c >= '0' && c <= '9') {
    char_info.key = std::string(1, c);
    char_info.code = std::string("Digit") + c;
    char_info.windows_virtual_key = c;
  } else if (c == ' ') {
    char_info.key = " ";
    char_info.code = "Space";
    char_info.windows_virtual_key = 32;
  } else if (c == '\n' || c == '\r') {
    char_info.key = "Enter";
    char_info.code = "Enter";
    char_info.text = "\r";
    char_info.windows_virtual_key = 13;
  } else if (c == '\t') {
    char_info.key = "Tab";
    char_info.code = "Tab";
    char_info.text = "\t";
    char_info.windows_virtual_key = 9;
  } else if (const UsKeyMapping* mapping = GetUsKeyMapping(c)) {
    // Symbol/punctuation with known US keyboard mapping.
    // Use the physical key's code and VK, with Shift if required.
    char_info.key = std::string(1, c);
    char_info.code = mapping->code;
    char_info.windows_virtual_key = mapping->windows_virtual_key;
    if (mapping->shift)
      web_mods = blink::WebInputEvent::kShiftKey;
  } else {
    // Unknown character — best effort: use character as key.
    char_info.key = std::string(1, c);
    char_info.windows_virtual_key = std::toupper(c);
  }
  char_info.native_virtual_key = char_info.windows_virtual_key;

  ForwardKeyEvent(ctx->web_contents(),
                  blink::WebInputEvent::Type::kKeyDown, char_info, web_mods);

  // Dwell time before keyUp, then inter-key pause before next character
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpInputDispatcher::TypeCharKeyUp,
                     base::Unretained(this), ctx, char_info, web_mods,
                     std::move(text), char_index),
      base::Milliseconds(kKeyDwellMs));
}

void AbpInputDispatcher::TypeCharKeyUp(scoped_refptr<AbpActionContext> ctx,
                                       KeyInfo info,
                                       int web_mods,
                                       std::string text,
                                       size_t char_index) {
  ForwardKeyEvent(ctx->web_contents(), blink::WebInputEvent::Type::kKeyUp,
                  info, web_mods);
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpInputDispatcher::TypeNextCharacter,
                     base::Unretained(this), ctx, std::move(text),
                     char_index + 1),
      base::Milliseconds(kInterKeyDelayMs));
}

void AbpInputDispatcher::Move(const std::string& tab_id,
                              const base::Value::Dict& params,
                              ResponseCallback callback) {
  // Validate params early.
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    controller_->SendError(400, "Missing 'x' or 'y' parameter",
                           std::move(callback));
    return;
  }

  double move_x = *x_opt;
  double move_y = *y_opt;

  // Use AbpActionContext for unified action flow.
  auto options = controller_->GetDefaultActionOptions();
  AbpActionContext::RunWithOptions(
      controller_, tab_id, "move", params, options,
      // Action callback - performs the cursor move.
      base::BindOnce(
          [](double coord_x, double coord_y, AbpActionContext* ctx) {
            // Update virtual cursor state via controller.
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), coord_x,
                                                        coord_y);

            // Enable and set virtual cursor via Mojo for on-screen rendering.
            // The renderer will detect cursor type via hit-testing in SetPosition().
            content::WebContents* wc = ctx->web_contents();
            if (wc) {
              ctx->controller()->SetVirtualCursorEnabledViaMojo(wc, true);
              ctx->controller()->SetVirtualCursorViaMojo(wc, coord_x, coord_y,
                                                          true);
            }

            // Take a scoped_refptr to keep context alive through async calls.
            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            ctx->controller()->InsertVisualStateFence(
                ctx->tab_id(),
                base::BindOnce(
                    [](double final_x, double final_y,
                       scoped_refptr<AbpActionContext> action_ctx,
                       bool fence_ready) {
                      if (!fence_ready) {
                        action_ctx->OnActionError(
                            "VISUAL_STATE_ERROR",
                            "Failed to establish visual-state fence before move");
                        return;
                      }

                      AbpCdpClient* cdp_client = action_ctx->client();
                      if (!cdp_client) {
                        action_ctx->OnActionError("CDP_ERROR",
                                                  "CDP client lost");
                        return;
                      }

                      // Send mouseMoved event for page interaction
                      // (hover states, etc.).
                      base::Value::Dict move_params;
                      move_params.Set("type", "mouseMoved");
                      move_params.Set("x", final_x);
                      move_params.Set("y", final_y);

                      VLOG(1) << "ABP Move: Sending Input.dispatchMouseEvent ("
                              << final_x << ", " << final_y << ")";
                      cdp_client->SendCommand(
                          "Input.dispatchMouseEvent", std::move(move_params),
                          base::BindOnce(
                              [](double final_x, double final_y,
                                 scoped_refptr<AbpActionContext> action_ctx,
                                 bool success, const std::string& result) {
                                VLOG(1) << "ABP Move: Input.dispatchMouseEvent callback, success="
                                        << success;
                                if (!success) {
                                  action_ctx->OnActionError("CDP_ERROR",
                                                            result);
                                  return;
                                }

                                base::Value::Dict res;
                                res.Set("status", "moved");
                                res.Set("x", final_x);
                                res.Set("y", final_y);
                                action_ctx->SetResult(std::move(res));
                                action_ctx->OnActionDispatched();
                              },
                              final_x, final_y, action_ctx));
                    },
                    coord_x, coord_y, std::move(ctx_ref)));
          },
          move_x, move_y),
      std::move(callback));
}

// DispatchMultiScroll dispatches up to 3 scrolls with 500ms delays between
// them (up to ~1000ms total). Both |controller| and |dispatcher| are raw
// pointers to objects owned by AbpController, which lives for the browser
// session lifetime — safe to hold across the PostDelayedTask chain.
// |ctx| is held via scoped_refptr so the action context stays alive.
static void DispatchMultiScroll(double x,
                                double y,
                                std::vector<std::pair<double, double>> scrolls,
                                size_t index,
                                base::Value::List intermediate_screenshots,
                                std::string tab_id,
                                AbpController* controller,
                                AbpInputDispatcher* dispatcher,
                                scoped_refptr<AbpActionContext> ctx) {
  content::WebContents* wc = ctx->web_contents();
  if (!wc) {
    ctx->OnActionError("TAB_ERROR", "WebContents lost");
    return;
  }

  // Move mouse to target coordinates first (like a real mouse would), then
  // scroll. This order matches actual user behavior and ensures hover state
  // is correct before the wheel event fires.
  dispatcher->ForwardMouseMoveEvent(wc, x, y);
  controller->UpdateVirtualCursorState(tab_id, x, y);
  controller->SetVirtualCursorEnabledViaMojo(wc, true);
  controller->SetVirtualCursorViaMojo(wc, x, y, true);

  auto [dx, dy] = scrolls[index];
  dispatcher->ForwardWheelEvent(wc, x, y, dx, dy);

  bool is_last = (index == scrolls.size() - 1);
  if (is_last) {
    // Final scroll: let AbpActionContext handle screenshot_after normally.
    base::Value::Dict res;
    res.Set("status", "scrolled");
    res.Set("scrolls_executed", static_cast<int>(scrolls.size()));
    res.Set("x", x);
    res.Set("y", y);
    if (!intermediate_screenshots.empty()) {
      res.Set("intermediate_screenshots", std::move(intermediate_screenshots));
    }
    ctx->SetResult(std::move(res));
    ctx->OnActionDispatched();
    return;
  }

  // Non-final scroll: wait 500ms for scroll to render, then capture.
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](double x, double y,
             std::vector<std::pair<double, double>> scrolls, size_t next_index,
             base::Value::List intermediate_screenshots, std::string tab_id,
             AbpController* controller, AbpInputDispatcher* dispatcher,
             scoped_refptr<AbpActionContext> ctx) {
            AbpController::ScreenshotOptions opts;
            // Use webp at quality 80 for intermediate screenshots.
            opts.format = "webp";
            opts.quality = 80;
            controller->CaptureScreenshotFromBuffer(
                tab_id, /*timestamp=*/0, /*is_before=*/false, opts,
                base::BindOnce(
                    [](double x, double y,
                       std::vector<std::pair<double, double>> scrolls,
                       size_t next_index,
                       base::Value::List intermediate_screenshots,
                       std::string tab_id, AbpController* controller,
                       AbpInputDispatcher* dispatcher,
                       scoped_refptr<AbpActionContext> ctx,
                       AbpController::ActionScreenshotResult result) {
                      if (!result.base64.empty()) {
                        base::Value::Dict ss;
                        ss.Set("data", std::move(result.base64));
                        ss.Set("width", result.width);
                        ss.Set("height", result.height);
                        ss.Set("format", "webp");
                        intermediate_screenshots.Append(std::move(ss));
                      }
                      DispatchMultiScroll(x, y, std::move(scrolls), next_index,
                                         std::move(intermediate_screenshots),
                                         std::move(tab_id), controller,
                                         dispatcher, std::move(ctx));
                    },
                    x, y, std::move(scrolls), next_index,
                    std::move(intermediate_screenshots), tab_id, controller,
                    dispatcher, std::move(ctx)));
          },
          x, y, std::move(scrolls), index + 1,
          std::move(intermediate_screenshots), std::move(tab_id), controller,
          dispatcher, std::move(ctx)),
      base::Milliseconds(500));
}

void AbpInputDispatcher::Scroll(const std::string& tab_id,
                                const base::Value::Dict& params,
                                ResponseCallback callback) {
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    controller_->SendError(400, "Missing required 'x' or 'y' parameter",
                           std::move(callback));
    return;
  }
  double x = *x_opt;
  double y = *y_opt;

  // scrolls is required: 1-3 {delta_px, direction} items.
  const base::Value::List* scrolls_list = params.FindList("scrolls");
  if (!scrolls_list || scrolls_list->empty()) {
    controller_->SendError(
        400, "'scrolls' is required and must not be empty",
        std::move(callback));
    return;
  }
  if (scrolls_list->size() > 3) {
    controller_->SendError(
        400, "'scrolls' array must have at most 3 elements",
        std::move(callback));
    return;
  }

  // Validate and map {delta_px, direction} → {delta_x, delta_y} pairs.
  std::vector<std::pair<double, double>> scrolls;
  scrolls.reserve(scrolls_list->size());
  for (size_t i = 0; i < scrolls_list->size(); i++) {
    const base::Value::Dict* item = (*scrolls_list)[i].GetIfDict();
    if (!item) {
      controller_->SendError(400, "Each 'scrolls' item must be an object",
                             std::move(callback));
      return;
    }
    auto delta_px = item->FindDouble("delta_px");
    const std::string* direction = item->FindString("direction");
    if (!delta_px || !direction) {
      controller_->SendError(
          400, "Each 'scrolls' item must have 'delta_px' and 'direction'",
          std::move(callback));
      return;
    }
    if (*direction != "x" && *direction != "y") {
      controller_->SendError(400, "'direction' must be 'x' or 'y'",
                             std::move(callback));
      return;
    }
    double dx = (*direction == "x") ? *delta_px : 0.0;
    double dy = (*direction == "y") ? *delta_px : 0.0;
    scrolls.emplace_back(dx, dy);
  }

  auto options = controller_->GetDefaultActionOptions();
  options.min_wait_time = base::Milliseconds(500);
  AbpActionContext::RunWithOptions(
      controller_, tab_id, "scroll", params, options,
      base::BindOnce(
          [](double scroll_x, double scroll_y,
             std::vector<std::pair<double, double>> scrolls,
             std::string tab_id, AbpController* controller,
             AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            DispatchMultiScroll(scroll_x, scroll_y, std::move(scrolls), 0,
                                base::Value::List(), std::move(tab_id),
                                controller, dispatcher, std::move(ctx_ref));
          },
          x, y, std::move(scrolls), tab_id, controller_, this),
      std::move(callback));
}

void AbpInputDispatcher::KeyPress(const std::string& tab_id,
                                  const base::Value::Dict& params,
                                  ResponseCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    controller_->SendError(400, "Missing 'key' parameter", std::move(callback));
    return;
  }

  // Get modifiers from params
  std::vector<std::string> modifiers;
  const base::Value::List* mod_list = params.FindList("modifiers");
  if (mod_list) {
    for (const auto& mod : *mod_list) {
      if (mod.is_string()) {
        modifiers.push_back(mod.GetString());
      }
    }
  }

  std::string key_copy = *key;

  // Use AbpActionContext for unified action flow.
  // KeyPress uses native keyboard events via ForwardKeyEvent — no CDP.
  // keyDown is dispatched immediately; keyUp follows after 30ms dwell time.
  AbpActionContext::RunWithOptions(
      controller_, tab_id, "key_press", params, controller_->GetDefaultActionOptions(),
      base::BindOnce(
          [](std::string pressed_key, std::vector<std::string> mods,
             AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
            content::WebContents* wc = ctx->web_contents();
            if (!wc) {
              ctx->OnActionError("TAB_ERROR", "WebContents lost");
              return;
            }

            KeyInfo key_info = GetKeyInfo(pressed_key);
            int mod_flags = ModifiersToFlags(mods);
            int web_mods = ModifierFlagsToWebModifiers(mod_flags);

            // Press modifier keys down
            for (const auto& mod_name : mods) {
              KeyInfo mod_info = GetKeyInfo(mod_name);
              dispatcher->ForwardKeyEvent(
                  wc, blink::WebInputEvent::Type::kKeyDown, mod_info,
                  web_mods);
            }

            // Press the main key down
            dispatcher->ForwardKeyEvent(
                wc, blink::WebInputEvent::Type::kKeyDown, key_info, web_mods);

            // Dwell time before keyUp + modifier release + action complete
            content::GetUIThreadTaskRunner({})->PostDelayedTask(
                FROM_HERE,
                base::BindOnce(&AbpInputDispatcher::KeyPressKeyUp,
                               base::Unretained(dispatcher),
                               base::Unretained(wc), key_info, web_mods,
                               std::move(mods), base::Unretained(ctx)),
                base::Milliseconds(kKeyDwellMs));
          },
          std::move(key_copy), std::move(modifiers), this),
      std::move(callback));
}

void AbpInputDispatcher::KeyDown(const std::string& tab_id,
                                 const base::Value::Dict& params,
                                 ResponseCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    controller_->SendError(400, "Missing 'key' parameter", std::move(callback));
    return;
  }

  std::string key_copy = *key;

  // Use AbpActionContext for unified action flow.
  // KeyDown uses native keyboard events — no CDP.
  AbpActionContext::RunWithOptions(
      controller_, tab_id, "key_down", params, controller_->GetDefaultActionOptions(),
      base::BindOnce(
          [](std::string pressed_key, AbpController* controller,
             AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
            content::WebContents* wc = ctx->web_contents();
            if (!wc) {
              ctx->OnActionError("TAB_ERROR", "WebContents lost");
              return;
            }

            KeyInfo key_info = GetKeyInfo(pressed_key);

            // Track the held key
            auto& held_state =
                controller->GetOrCreateTabState(ctx->tab_id()).held_keys;
            held_state.held_keys.insert(pressed_key);
            if (key_info.is_modifier) {
              held_state.current_modifiers |= key_info.modifier_flag;
            }

            int web_mods =
                ModifierFlagsToWebModifiers(held_state.current_modifiers);

            dispatcher->ForwardKeyEvent(
                wc, blink::WebInputEvent::Type::kKeyDown, key_info, web_mods);

            base::Value::Dict res;
            res.Set("status", "key_down");
            res.Set("key", pressed_key);
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          std::move(key_copy), controller_, this),
      std::move(callback));
}

void AbpInputDispatcher::KeyUp(const std::string& tab_id,
                               const base::Value::Dict& params,
                               ResponseCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    controller_->SendError(400, "Missing 'key' parameter", std::move(callback));
    return;
  }

  std::string key_copy = *key;

  // Use AbpActionContext for unified action flow.
  // KeyUp uses native keyboard events — no CDP.
  AbpActionContext::RunWithOptions(
      controller_, tab_id, "key_up", params, controller_->GetDefaultActionOptions(),
      base::BindOnce(
          [](std::string released_key, AbpController* controller,
             AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
            content::WebContents* wc = ctx->web_contents();
            if (!wc) {
              ctx->OnActionError("TAB_ERROR", "WebContents lost");
              return;
            }

            KeyInfo key_info = GetKeyInfo(released_key);

            // Update held key tracking
            auto& held_state =
                controller->GetOrCreateTabState(ctx->tab_id()).held_keys;
            held_state.held_keys.erase(released_key);
            if (key_info.is_modifier) {
              held_state.current_modifiers &= ~key_info.modifier_flag;
            }

            int web_mods =
                ModifierFlagsToWebModifiers(held_state.current_modifiers);

            dispatcher->ForwardKeyEvent(
                wc, blink::WebInputEvent::Type::kKeyUp, key_info, web_mods);

            base::Value::Dict res;
            res.Set("status", "key_up");
            res.Set("key", released_key);
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          std::move(key_copy), controller_, this),
      std::move(callback));
}

void AbpInputDispatcher::Drag(const std::string& tab_id,
                              const base::Value::Dict& params,
                              ResponseCallback callback) {
  auto sx = params.FindDouble("start_x");
  auto sy = params.FindDouble("start_y");
  auto ex = params.FindDouble("end_x");
  auto ey = params.FindDouble("end_y");
  if (!sx || !sy || !ex || !ey) {
    controller_->SendError(
        400, "Missing required parameter: start_x, start_y, end_x, end_y",
        std::move(callback));
    return;
  }

  double start_x = *sx;
  double start_y = *sy;
  double end_x = *ex;
  double end_y = *ey;
  int steps = params.FindInt("steps").value_or(50);
  if (steps < 1) steps = 1;
  if (steps > 200) steps = 200;

  AbpActionContext::RunWithOptions(
      controller_, tab_id, "drag", params, controller_->GetDefaultActionOptions(),
      base::BindOnce(
          [](double s_x, double s_y, double e_x, double e_y, int num_steps,
             AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
            // Update virtual cursor to start position
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), s_x,
                                                        s_y);
            content::WebContents* wc = ctx->web_contents();
            if (wc) {
              ctx->controller()->SetVirtualCursorEnabledViaMojo(wc, true);
              ctx->controller()->SetVirtualCursorViaMojo(wc, s_x, s_y, true);
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            ctx->controller()->InsertVisualStateFence(
                ctx->tab_id(),
                base::BindOnce(
                    [](double s_x, double s_y, double e_x, double e_y,
                       int num_steps, AbpInputDispatcher* dispatcher,
                       scoped_refptr<AbpActionContext> action_ctx,
                       bool fence_ready) {
                      if (!fence_ready) {
                        action_ctx->OnActionError(
                            "VISUAL_STATE_ERROR",
                            "Failed to establish visual-state fence before drag");
                        return;
                      }

                      AbpCdpClient* cdp_client = action_ctx->client();
                      if (!cdp_client) {
                        action_ctx->OnActionError("CDP_ERROR",
                                                  "CDP client lost");
                        return;
                      }

                      // 1. mouseMoved to start position
                      base::Value::Dict move_params;
                      move_params.Set("type", "mouseMoved");
                      move_params.Set("x", s_x);
                      move_params.Set("y", s_y);

                      cdp_client->SendCommand(
                          "Input.dispatchMouseEvent", std::move(move_params),
                          base::BindOnce(
                              [](double s_x, double s_y, double e_x,
                                 double e_y, int num_steps,
                                 AbpInputDispatcher* dispatcher,
                                 scoped_refptr<AbpActionContext> action_ctx,
                                 bool success, const std::string& result) {
                                if (!success) {
                                  action_ctx->OnActionError("CDP_ERROR",
                                                            result);
                                  return;
                                }

                                AbpCdpClient* cdp_client =
                                    action_ctx->client();
                                if (!cdp_client) {
                                  action_ctx->OnActionError("CDP_ERROR",
                                                            "CDP client lost");
                                  return;
                                }

                                // 2. mousePressed at start
                                base::Value::Dict press_params;
                                press_params.Set("type", "mousePressed");
                                press_params.Set("x", s_x);
                                press_params.Set("y", s_y);
                                press_params.Set("button", "left");
                                press_params.Set("clickCount", 1);

                                cdp_client->SendCommand(
                                    "Input.dispatchMouseEvent",
                                    std::move(press_params),
                                    base::BindOnce(
                                        [](double s_x, double s_y, double e_x,
                                           double e_y, int num_steps,
                                           AbpInputDispatcher* dispatcher,
                                           scoped_refptr<AbpActionContext>
                                               action_ctx,
                                           bool success,
                                           const std::string& result) {
                                          if (!success) {
                                            action_ctx->OnActionError(
                                                "CDP_ERROR", result);
                                            return;
                                          }

                                          // 3. Start interpolated moves
                                          dispatcher->DragNextStep(
                                              action_ctx, s_x, s_y, e_x, e_y,
                                              1, num_steps);
                                        },
                                        s_x, s_y, e_x, e_y, num_steps,
                                        dispatcher, action_ctx));
                              },
                              s_x, s_y, e_x, e_y, num_steps, dispatcher,
                              action_ctx));
                    },
                    s_x, s_y, e_x, e_y, num_steps, dispatcher,
                    std::move(ctx_ref)));
          },
          start_x, start_y, end_x, end_y, steps, this),
      std::move(callback));
}

void AbpInputDispatcher::Slider(const std::string& tab_id,
                                const base::Value::Dict& params,
                                ResponseCallback callback) {
  const std::string* orientation = params.FindString("orientation");
  if (!orientation ||
      (*orientation != "horizontal" && *orientation != "vertical")) {
    controller_->SendError(
        400, "orientation must be 'horizontal' or 'vertical'",
        std::move(callback));
    return;
  }

  auto min_val = params.FindDouble("min");
  auto max_val = params.FindDouble("max");
  auto target = params.FindDouble("target_value");
  if (!min_val || !max_val || !target) {
    controller_->SendError(
        400, "Missing required parameter: min, max, target_value",
        std::move(callback));
    return;
  }
  if (*min_val >= *max_val) {
    controller_->SendError(400, "min must be less than max",
                           std::move(callback));
    return;
  }
  if (*target < *min_val || *target > *max_val) {
    controller_->SendError(400, "target_value must be between min and max",
                           std::move(callback));
    return;
  }

  double ratio = (*target - *min_val) / (*max_val - *min_val);
  double start_x, start_y, end_x, end_y;

  if (*orientation == "horizontal") {
    auto y = params.FindDouble("y");
    auto x_start = params.FindDouble("x_start");
    auto x_end = params.FindDouble("x_end");
    auto current_x = params.FindDouble("current_x");
    if (!y || !x_start || !x_end || !current_x) {
      controller_->SendError(
          400,
          "horizontal orientation requires y, x_start, x_end, current_x",
          std::move(callback));
      return;
    }
    if (*x_start == *x_end) {
      controller_->SendError(400, "x_start and x_end must be different",
                             std::move(callback));
      return;
    }
    double lo = std::min(*x_start, *x_end);
    double hi = std::max(*x_start, *x_end);
    if (*current_x < lo || *current_x > hi) {
      controller_->SendError(
          400, "current_x must be within track bounds (x_start to x_end)",
          std::move(callback));
      return;
    }
    double target_x = *x_start + ratio * (*x_end - *x_start);
    target_x = std::clamp(target_x, lo, hi);
    start_x = *current_x;
    start_y = *y;
    end_x = target_x;
    end_y = *y;
  } else {
    auto x = params.FindDouble("x");
    auto y_start = params.FindDouble("y_start");
    auto y_end = params.FindDouble("y_end");
    auto current_y = params.FindDouble("current_y");
    if (!x || !y_start || !y_end || !current_y) {
      controller_->SendError(
          400,
          "vertical orientation requires x, y_start, y_end, current_y",
          std::move(callback));
      return;
    }
    if (*y_start == *y_end) {
      controller_->SendError(400, "y_start and y_end must be different",
                             std::move(callback));
      return;
    }
    double lo = std::min(*y_start, *y_end);
    double hi = std::max(*y_start, *y_end);
    if (*current_y < lo || *current_y > hi) {
      controller_->SendError(
          400, "current_y must be within track bounds (y_start to y_end)",
          std::move(callback));
      return;
    }
    double target_y = *y_start + ratio * (*y_end - *y_start);
    target_y = std::clamp(target_y, lo, hi);
    start_x = *x;
    start_y = *current_y;
    end_x = *x;
    end_y = target_y;
  }

  // Build drag params and delegate to Drag()
  base::Value::Dict drag_params;
  drag_params.Set("start_x", start_x);
  drag_params.Set("start_y", start_y);
  drag_params.Set("end_x", end_x);
  drag_params.Set("end_y", end_y);
  drag_params.Set("steps", 50);

  // Use Drag() which handles AbpActionContext lifecycle
  Drag(tab_id, drag_params, std::move(callback));
}

void AbpInputDispatcher::ClearText(const std::string& tab_id,
                                   const base::Value::Dict& params,
                                   ResponseCallback callback) {
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    controller_->SendError(400, "Missing required 'x' or 'y' parameter",
                           std::move(callback));
    return;
  }

  double click_x = *x_opt;
  double click_y = *y_opt;

  AbpActionContext::RunWithOptions(
      controller_, tab_id, "clear_text", params,
      controller_->GetDefaultActionOptions(),
      base::BindOnce(
          [](double x, double y, AbpInputDispatcher* dispatcher,
             AbpActionContext* ctx) {
            // Update virtual cursor to click position
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), x, y);
            content::WebContents* wc = ctx->web_contents();
            if (wc) {
              ctx->controller()->SetVirtualCursorEnabledViaMojo(wc, true);
              ctx->controller()->SetVirtualCursorViaMojo(wc, x, y, true);
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            ctx->controller()->InsertVisualStateFence(
                ctx->tab_id(),
                base::BindOnce(
                    [](double x, double y, AbpInputDispatcher* disp,
                       scoped_refptr<AbpActionContext> action_ctx,
                       bool fence_ready) {
                      if (!fence_ready) {
                        action_ctx->OnActionError(
                            "VISUAL_STATE_ERROR",
                            "Failed to establish visual-state fence");
                        return;
                      }

                      AbpCdpClient* cdp_client = action_ctx->client();
                      if (!cdp_client) {
                        action_ctx->OnActionError("CDP_ERROR",
                                                  "CDP client lost");
                        return;
                      }

                      // Click to focus: mousePressed
                      base::Value::Dict press_params;
                      press_params.Set("type", "mousePressed");
                      press_params.Set("x", x);
                      press_params.Set("y", y);
                      press_params.Set("button", "left");
                      press_params.Set("clickCount", 1);

                      cdp_client->SendCommand(
                          "Input.dispatchMouseEvent", std::move(press_params),
                          base::BindOnce(
                              [](double x, double y,
                                 AbpInputDispatcher* disp,
                                 scoped_refptr<AbpActionContext> action_ctx,
                                 bool success, const std::string& result) {
                                if (!success) {
                                  action_ctx->OnActionError("CDP_ERROR",
                                                            result);
                                  return;
                                }

                                AbpCdpClient* cdp_client =
                                    action_ctx->client();
                                if (!cdp_client) {
                                  action_ctx->OnActionError("CDP_ERROR",
                                                            "CDP client lost");
                                  return;
                                }

                                // Click to focus: mouseReleased
                                base::Value::Dict release_params;
                                release_params.Set("type", "mouseReleased");
                                release_params.Set("x", x);
                                release_params.Set("y", y);
                                release_params.Set("button", "left");
                                release_params.Set("clickCount", 1);

                                cdp_client->SendCommand(
                                    "Input.dispatchMouseEvent",
                                    std::move(release_params),
                                    base::BindOnce(
                                        [](AbpInputDispatcher* disp,
                                           scoped_refptr<AbpActionContext>
                                               action_ctx,
                                           bool success,
                                           const std::string& result) {
                                          if (!success) {
                                            action_ctx->OnActionError(
                                                "CDP_ERROR", result);
                                            return;
                                          }

                                          // Click done — SelectAll + Backspace
                                          content::WebContents* wc =
                                              action_ctx->web_contents();
                                          if (!wc) {
                                            action_ctx->OnActionError(
                                                "TAB_ERROR",
                                                "WebContents lost");
                                            return;
                                          }

                                          // Select all text in focused element
                                          wc->SelectAll();

                                          // Send Backspace keyDown to delete
                                          // the selection
                                          KeyInfo bs = GetKeyInfo("Backspace");
                                          disp->ForwardKeyEvent(
                                              wc,
                                              blink::WebInputEvent::Type::
                                                  kKeyDown,
                                              bs, 0);

                                          // Schedule keyUp after dwell
                                          content::GetUIThreadTaskRunner(
                                              {})->PostDelayedTask(
                                              FROM_HERE,
                                              base::BindOnce(
                                                  &AbpInputDispatcher::
                                                      ClearTextBackspaceUp,
                                                  base::Unretained(disp),
                                                  action_ctx),
                                              base::Milliseconds(kKeyDwellMs));
                                        },
                                        disp, action_ctx));
                              },
                              x, y, disp, action_ctx));
                    },
                    x, y, dispatcher, std::move(ctx_ref)));
          },
          click_x, click_y, this),
      std::move(callback));
}

void AbpInputDispatcher::ClearTextBackspaceUp(
    scoped_refptr<AbpActionContext> ctx) {
  content::WebContents* wc = ctx->web_contents();
  if (!wc) {
    ctx->OnActionError("TAB_ERROR", "WebContents lost");
    return;
  }

  KeyInfo bs = GetKeyInfo("Backspace");
  ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyUp, bs, 0);

  base::Value::Dict res;
  res.Set("status", "cleared");
  ctx->SetResult(std::move(res));
  ctx->OnActionDispatched();
}

void AbpInputDispatcher::DragNextStep(
    scoped_refptr<AbpActionContext> ctx,
    double start_x,
    double start_y,
    double end_x,
    double end_y,
    int current_step,
    int total_steps) {
  AbpCdpClient* cdp_client = ctx->client();
  if (!cdp_client) {
    ctx->OnActionError("CDP_ERROR", "CDP client lost");
    return;
  }

  if (current_step <= total_steps) {
    // Interpolate position
    double t = static_cast<double>(current_step) / total_steps;
    double x = start_x + (end_x - start_x) * t;
    double y = start_y + (end_y - start_y) * t;

    // Update virtual cursor as we drag
    ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), x, y);
    content::WebContents* wc = ctx->web_contents();
    if (wc) {
      ctx->controller()->SetVirtualCursorViaMojo(wc, x, y, true);
    }

    base::Value::Dict move_params;
    move_params.Set("type", "mouseMoved");
    move_params.Set("x", x);
    move_params.Set("y", y);
    move_params.Set("button", "left");

    cdp_client->SendCommand(
        "Input.dispatchMouseEvent", std::move(move_params),
        base::BindOnce(
            [](scoped_refptr<AbpActionContext> ctx, double s_x, double s_y,
               double e_x, double e_y, int step, int total,
               AbpInputDispatcher* dispatcher, bool success,
               const std::string& result) {
              if (!success) {
                ctx->OnActionError("CDP_ERROR", result);
                return;
              }

              // Schedule next step with 10ms delay
              content::GetUIThreadTaskRunner({})->PostDelayedTask(
                  FROM_HERE,
                  base::BindOnce(&AbpInputDispatcher::DragNextStep,
                                 base::Unretained(dispatcher), ctx, s_x, s_y,
                                 e_x, e_y, step + 1, total),
                  base::Milliseconds(10));
            },
            ctx, start_x, start_y, end_x, end_y, current_step, total_steps,
            this));
  } else {
    // All steps done — send mouseReleased at end position
    ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), end_x, end_y);
    content::WebContents* wc = ctx->web_contents();
    if (wc) {
      ctx->controller()->SetVirtualCursorViaMojo(wc, end_x, end_y, true);
    }

    base::Value::Dict release_params;
    release_params.Set("type", "mouseReleased");
    release_params.Set("x", end_x);
    release_params.Set("y", end_y);
    release_params.Set("button", "left");
    release_params.Set("clickCount", 1);

    cdp_client->SendCommand(
        "Input.dispatchMouseEvent", std::move(release_params),
        base::BindOnce(
            [](double s_x, double s_y, double e_x, double e_y,
               scoped_refptr<AbpActionContext> ctx, bool success,
               const std::string& result) {
              if (!success) {
                ctx->OnActionError("CDP_ERROR", result);
                return;
              }

              base::Value::Dict res;
              res.Set("status", "dragged");
              res.Set("start_x", s_x);
              res.Set("start_y", s_y);
              res.Set("end_x", e_x);
              res.Set("end_y", e_y);
              ctx->SetResult(std::move(res));
              ctx->OnActionDispatched();
            },
            start_x, start_y, end_x, end_y, ctx));
  }
}

// ==========================================================================
// Raw dispatch methods — no AbpActionContext, no visual state fence.
// Used within batch execution where a single action context wraps all actions.
// ==========================================================================

void AbpInputDispatcher::ClickRaw(const std::string& tab_id,
                                  const base::Value::Dict& params,
                                  RawCallback callback) {
  double x = params.FindDouble("x").value_or(0);
  double y = params.FindDouble("y").value_or(0);

  const std::string* button_param = params.FindString("button");
  std::string button = (button_param && (*button_param == "right" ||
                                          *button_param == "middle"))
                            ? *button_param
                            : "left";

  int click_count = params.FindInt("click_count").value_or(1);
  if (click_count < 1) click_count = 1;
  if (click_count > 3) click_count = 3;

  int mod_flags = 0;
  const base::Value::List* mod_list = params.FindList("modifiers");
  if (mod_list) {
    std::vector<std::string> modifiers;
    for (const auto& mod : *mod_list) {
      if (mod.is_string())
        modifiers.push_back(mod.GetString());
    }
    mod_flags = ModifiersToFlags(modifiers);
  }

  // Update virtual cursor
  controller_->UpdateVirtualCursorState(tab_id, x, y);
  content::WebContents* wc = controller_->FindWebContents(tab_id);
  if (wc) {
    controller_->SetVirtualCursorEnabledViaMojo(wc, true);
    controller_->SetVirtualCursorViaMojo(wc, x, y, true);
  }

  AbpCdpClient* cdp_client = wc ? controller_->GetOrCreateCdpClient(wc)
                                 : nullptr;
  if (!cdp_client) {
    std::move(callback).Run();
    return;
  }

  // Convert agent DIP coordinates to the CSS pixels CDP expects (see
  // AbpPageZoomFactor). The virtual cursor above stays in DIP.
  double zoom_factor = AbpPageZoomFactor(wc);
  double cdp_x = x / zoom_factor;
  double cdp_y = y / zoom_factor;

  // Send mousePressed
  base::Value::Dict press_params;
  press_params.Set("type", "mousePressed");
  press_params.Set("x", cdp_x);
  press_params.Set("y", cdp_y);
  press_params.Set("button", button);
  press_params.Set("clickCount", click_count);
  press_params.Set("modifiers", mod_flags);

  cdp_client->SendCommand(
      "Input.dispatchMouseEvent", std::move(press_params),
      base::BindOnce(
          [](double x, double y, std::string button, int click_count,
             int mods, std::string tab_id, AbpInputDispatcher* dispatcher,
             AbpInputDispatcher::RawCallback callback,
             bool success, const std::string& result) {
            if (!success) {
              std::move(callback).Run();
              return;
            }

            // Re-resolve CDP client for mouseReleased (tab may have closed)
            content::WebContents* wc =
                dispatcher->controller_->FindWebContents(tab_id);
            AbpCdpClient* client =
                wc ? dispatcher->controller_->GetOrCreateCdpClient(wc)
                   : nullptr;
            if (!client) {
              std::move(callback).Run();
              return;
            }

            // Send mouseReleased
            base::Value::Dict release_params;
            release_params.Set("type", "mouseReleased");
            release_params.Set("x", x);
            release_params.Set("y", y);
            release_params.Set("button", button);
            release_params.Set("clickCount", click_count);
            release_params.Set("modifiers", mods);

            client->SendCommand(
                "Input.dispatchMouseEvent", std::move(release_params),
                base::BindOnce(
                    [](AbpInputDispatcher::RawCallback callback,
                       bool success, const std::string& result) {
                      std::move(callback).Run();
                    },
                    std::move(callback)));
          },
          cdp_x, cdp_y, std::move(button), click_count, mod_flags,
          tab_id, this, std::move(callback)));
}

void AbpInputDispatcher::TypeRaw(const std::string& tab_id,
                                 const base::Value::Dict& params,
                                 RawCallback callback) {
  const std::string* text = params.FindString("text");
  if (!text || text->empty()) {
    std::move(callback).Run();
    return;
  }

  content::WebContents* wc = controller_->FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run();
    return;
  }

  TypeNextCharacterRaw(tab_id, *text, 0, std::move(callback));
}

void AbpInputDispatcher::TypeNextCharacterRaw(
    const std::string& tab_id,
    std::string text,
    size_t char_index,
    RawCallback callback) {
  if (char_index >= text.size()) {
    std::move(callback).Run();
    return;
  }

  // Re-resolve WebContents each iteration (tab may have been closed)
  content::WebContents* wc = controller_->FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run();
    return;
  }

  char c = text[char_index];
  int web_mods = 0;

  KeyInfo char_info;
  char_info.text = std::string(1, c);

  if (c >= 'a' && c <= 'z') {
    char_info.key = std::string(1, c);
    char_info.code = std::string("Key") + static_cast<char>(std::toupper(c));
    char_info.windows_virtual_key = std::toupper(c);
  } else if (c >= 'A' && c <= 'Z') {
    char_info.key = std::string(1, c);
    char_info.code = std::string("Key") + c;
    char_info.windows_virtual_key = c;
    web_mods = blink::WebInputEvent::kShiftKey;
  } else if (c >= '0' && c <= '9') {
    char_info.key = std::string(1, c);
    char_info.code = std::string("Digit") + c;
    char_info.windows_virtual_key = c;
  } else if (c == ' ') {
    char_info.key = " ";
    char_info.code = "Space";
    char_info.windows_virtual_key = 32;
  } else if (c == '\n' || c == '\r') {
    char_info.key = "Enter";
    char_info.code = "Enter";
    char_info.text = "\r";
    char_info.windows_virtual_key = 13;
  } else if (c == '\t') {
    char_info.key = "Tab";
    char_info.code = "Tab";
    char_info.text = "\t";
    char_info.windows_virtual_key = 9;
  } else if (const UsKeyMapping* mapping = GetUsKeyMapping(c)) {
    char_info.key = std::string(1, c);
    char_info.code = mapping->code;
    char_info.windows_virtual_key = mapping->windows_virtual_key;
    if (mapping->shift)
      web_mods = blink::WebInputEvent::kShiftKey;
  } else {
    char_info.key = std::string(1, c);
    char_info.windows_virtual_key = std::toupper(c);
  }
  char_info.native_virtual_key = char_info.windows_virtual_key;

  ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyDown, char_info,
                  web_mods);

  // Dwell time before keyUp, then inter-key pause before next character
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpInputDispatcher::TypeCharKeyUpRaw,
                     base::Unretained(this), base::Unretained(wc), char_info,
                     web_mods, tab_id, std::move(text), char_index,
                     std::move(callback)),
      base::Milliseconds(kKeyDwellMs));
}

void AbpInputDispatcher::TypeCharKeyUpRaw(content::WebContents* wc,
                                          KeyInfo info,
                                          int web_mods,
                                          std::string tab_id,
                                          std::string text,
                                          size_t char_index,
                                          RawCallback callback) {
  ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyUp, info, web_mods);
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpInputDispatcher::TypeNextCharacterRaw,
                     base::Unretained(this), std::move(tab_id),
                     std::move(text), char_index + 1, std::move(callback)),
      base::Milliseconds(kInterKeyDelayMs));
}

void AbpInputDispatcher::MoveRaw(const std::string& tab_id,
                                 const base::Value::Dict& params,
                                 RawCallback callback) {
  double x = params.FindDouble("x").value_or(0);
  double y = params.FindDouble("y").value_or(0);

  // Update virtual cursor
  controller_->UpdateVirtualCursorState(tab_id, x, y);
  content::WebContents* wc = controller_->FindWebContents(tab_id);
  if (wc) {
    controller_->SetVirtualCursorEnabledViaMojo(wc, true);
    controller_->SetVirtualCursorViaMojo(wc, x, y, true);
  }

  AbpCdpClient* cdp_client = wc ? controller_->GetOrCreateCdpClient(wc)
                                 : nullptr;
  if (!cdp_client) {
    std::move(callback).Run();
    return;
  }

  base::Value::Dict move_params;
  move_params.Set("type", "mouseMoved");
  move_params.Set("x", x);
  move_params.Set("y", y);

  cdp_client->SendCommand(
      "Input.dispatchMouseEvent", std::move(move_params),
      base::BindOnce(
          [](AbpInputDispatcher::RawCallback callback,
             bool success, const std::string& result) {
            std::move(callback).Run();
          },
          std::move(callback)));
}

void AbpInputDispatcher::KeyPressRaw(const std::string& tab_id,
                                     const base::Value::Dict& params,
                                     RawCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    std::move(callback).Run();
    return;
  }

  content::WebContents* wc = controller_->FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run();
    return;
  }

  std::vector<std::string> modifiers;
  const base::Value::List* mod_list = params.FindList("modifiers");
  if (mod_list) {
    for (const auto& mod : *mod_list) {
      if (mod.is_string())
        modifiers.push_back(mod.GetString());
    }
  }

  KeyInfo key_info = GetKeyInfo(*key);
  int mod_flags = ModifiersToFlags(modifiers);
  int web_mods = ModifierFlagsToWebModifiers(mod_flags);

  // Press modifier keys down
  for (const auto& mod_name : modifiers) {
    KeyInfo mod_info = GetKeyInfo(mod_name);
    ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyDown, mod_info,
                    web_mods);
  }

  // Press the main key down
  ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyDown, key_info,
                  web_mods);

  // Dwell time before keyUp + modifier release
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpInputDispatcher::KeyPressKeyUpRaw,
                     base::Unretained(this), base::Unretained(wc), key_info,
                     web_mods, std::move(modifiers), std::move(callback)),
      base::Milliseconds(kKeyDwellMs));
}

void AbpInputDispatcher::KeyPressKeyUp(content::WebContents* wc,
                                       KeyInfo key_info,
                                       int web_mods,
                                       std::vector<std::string> mods,
                                       AbpActionContext* ctx) {
  ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyUp, key_info, web_mods);

  // Release modifier keys in reverse order
  for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
    KeyInfo mod_info = GetKeyInfo(*it);
    ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyUp, mod_info, 0);
  }

  base::Value::Dict res;
  res.Set("status", "pressed");
  res.Set("key", key_info.key);
  if (!mods.empty()) {
    base::Value::List mod_result;
    for (const auto& m : mods) {
      mod_result.Append(m);
    }
    res.Set("modifiers", std::move(mod_result));
  }
  ctx->SetResult(std::move(res));
  ctx->OnActionDispatched();
}

void AbpInputDispatcher::KeyPressKeyUpRaw(
    content::WebContents* wc,
    KeyInfo key_info,
    int web_mods,
    std::vector<std::string> modifiers,
    RawCallback callback) {
  ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyUp, key_info, web_mods);

  // Release modifier keys in reverse order
  for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) {
    KeyInfo mod_info = GetKeyInfo(*it);
    ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyUp, mod_info, 0);
  }

  std::move(callback).Run();
}

void AbpInputDispatcher::KeyDownRaw(const std::string& tab_id,
                                    const base::Value::Dict& params,
                                    RawCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    std::move(callback).Run();
    return;
  }

  content::WebContents* wc = controller_->FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run();
    return;
  }

  KeyInfo key_info = GetKeyInfo(*key);

  // Track the held key
  auto& held_state =
      controller_->GetOrCreateTabState(tab_id).held_keys;
  held_state.held_keys.insert(*key);
  if (key_info.is_modifier) {
    held_state.current_modifiers |= key_info.modifier_flag;
  }

  int web_mods = ModifierFlagsToWebModifiers(held_state.current_modifiers);
  ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyDown, key_info,
                  web_mods);

  std::move(callback).Run();
}

void AbpInputDispatcher::KeyUpRaw(const std::string& tab_id,
                                  const base::Value::Dict& params,
                                  RawCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    std::move(callback).Run();
    return;
  }

  content::WebContents* wc = controller_->FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run();
    return;
  }

  KeyInfo key_info = GetKeyInfo(*key);

  // Update held key tracking
  auto& held_state =
      controller_->GetOrCreateTabState(tab_id).held_keys;
  held_state.held_keys.erase(*key);
  if (key_info.is_modifier) {
    held_state.current_modifiers &= ~key_info.modifier_flag;
  }

  int web_mods = ModifierFlagsToWebModifiers(held_state.current_modifiers);
  ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyUp, key_info, web_mods);

  std::move(callback).Run();
}

void AbpInputDispatcher::DragRaw(const std::string& tab_id,
                                 const base::Value::Dict& params,
                                 RawCallback callback) {
  double start_x = params.FindDouble("start_x").value_or(0);
  double start_y = params.FindDouble("start_y").value_or(0);
  double end_x = params.FindDouble("end_x").value_or(0);
  double end_y = params.FindDouble("end_y").value_or(0);
  int steps = params.FindInt("steps").value_or(50);
  if (steps < 1) steps = 1;
  if (steps > 200) steps = 200;

  // Update virtual cursor to start position
  controller_->UpdateVirtualCursorState(tab_id, start_x, start_y);
  content::WebContents* wc = controller_->FindWebContents(tab_id);
  if (wc) {
    controller_->SetVirtualCursorEnabledViaMojo(wc, true);
    controller_->SetVirtualCursorViaMojo(wc, start_x, start_y, true);
  }

  AbpCdpClient* cdp_client = wc ? controller_->GetOrCreateCdpClient(wc)
                                 : nullptr;
  if (!cdp_client) {
    std::move(callback).Run();
    return;
  }

  // 1. mouseMoved to start position
  base::Value::Dict move_params;
  move_params.Set("type", "mouseMoved");
  move_params.Set("x", start_x);
  move_params.Set("y", start_y);

  cdp_client->SendCommand(
      "Input.dispatchMouseEvent", std::move(move_params),
      base::BindOnce(
          [](std::string tab_id, double s_x, double s_y, double e_x,
             double e_y, int num_steps, AbpInputDispatcher* dispatcher,
             AbpInputDispatcher::RawCallback callback,
             bool success, const std::string& result) {
            if (!success) {
              std::move(callback).Run();
              return;
            }

            // Re-resolve CDP client (tab may have closed)
            content::WebContents* wc =
                dispatcher->controller_->FindWebContents(tab_id);
            AbpCdpClient* client =
                wc ? dispatcher->controller_->GetOrCreateCdpClient(wc)
                   : nullptr;
            if (!client) {
              std::move(callback).Run();
              return;
            }

            // 2. mousePressed at start
            base::Value::Dict press_params;
            press_params.Set("type", "mousePressed");
            press_params.Set("x", s_x);
            press_params.Set("y", s_y);
            press_params.Set("button", "left");
            press_params.Set("clickCount", 1);

            client->SendCommand(
                "Input.dispatchMouseEvent", std::move(press_params),
                base::BindOnce(
                    [](std::string tab_id, double s_x, double s_y, double e_x,
                       double e_y, int num_steps, AbpInputDispatcher* dispatcher,
                       AbpInputDispatcher::RawCallback callback,
                       bool success, const std::string& result) {
                      if (!success) {
                        std::move(callback).Run();
                        return;
                      }

                      // 3. Start interpolated moves
                      dispatcher->DragNextStepRaw(
                          tab_id, s_x, s_y, e_x, e_y,
                          1, num_steps, std::move(callback));
                    },
                    std::move(tab_id), s_x, s_y, e_x, e_y, num_steps,
                    dispatcher, std::move(callback)));
          },
          tab_id, start_x, start_y, end_x, end_y, steps,
          this, std::move(callback)));
}

void AbpInputDispatcher::DragNextStepRaw(
    const std::string& tab_id,
    double start_x,
    double start_y,
    double end_x,
    double end_y,
    int current_step,
    int total_steps,
    RawCallback callback) {
  // Re-resolve WebContents and CDP client each step (tab may have closed)
  content::WebContents* wc = controller_->FindWebContents(tab_id);
  AbpCdpClient* cdp_client =
      wc ? controller_->GetOrCreateCdpClient(wc) : nullptr;
  if (!cdp_client) {
    std::move(callback).Run();
    return;
  }

  if (current_step <= total_steps) {
    // Interpolate position
    double t = static_cast<double>(current_step) / total_steps;
    double x = start_x + (end_x - start_x) * t;
    double y = start_y + (end_y - start_y) * t;

    // Update virtual cursor as we drag
    controller_->UpdateVirtualCursorState(tab_id, x, y);
    if (wc) {
      controller_->SetVirtualCursorViaMojo(wc, x, y, true);
    }

    base::Value::Dict move_params;
    move_params.Set("type", "mouseMoved");
    move_params.Set("x", x);
    move_params.Set("y", y);
    move_params.Set("button", "left");

    cdp_client->SendCommand(
        "Input.dispatchMouseEvent", std::move(move_params),
        base::BindOnce(
            [](std::string tab_id,
               double s_x, double s_y, double e_x, double e_y,
               int step, int total, AbpInputDispatcher* dispatcher,
               AbpInputDispatcher::RawCallback callback,
               bool success, const std::string& result) {
              if (!success) {
                std::move(callback).Run();
                return;
              }

              // Schedule next step with 5ms delay
              content::GetUIThreadTaskRunner({})->PostDelayedTask(
                  FROM_HERE,
                  base::BindOnce(&AbpInputDispatcher::DragNextStepRaw,
                                 base::Unretained(dispatcher),
                                 std::move(tab_id),
                                 s_x, s_y, e_x, e_y,
                                 step + 1, total, std::move(callback)),
                  base::Milliseconds(10));
            },
            tab_id, start_x, start_y, end_x, end_y,
            current_step, total_steps, this, std::move(callback)));
  } else {
    // All steps done — send mouseReleased at end position
    controller_->UpdateVirtualCursorState(tab_id, end_x, end_y);
    if (wc) {
      controller_->SetVirtualCursorViaMojo(wc, end_x, end_y, true);
    }

    base::Value::Dict release_params;
    release_params.Set("type", "mouseReleased");
    release_params.Set("x", end_x);
    release_params.Set("y", end_y);
    release_params.Set("button", "left");
    release_params.Set("clickCount", 1);

    cdp_client->SendCommand(
        "Input.dispatchMouseEvent", std::move(release_params),
        base::BindOnce(
            [](AbpInputDispatcher::RawCallback callback,
               bool success, const std::string& result) {
              std::move(callback).Run();
            },
            std::move(callback)));
  }
}

}  // namespace abp
