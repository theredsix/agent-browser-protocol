// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_controller.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

#include "base/base64.h"
#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/no_destructor.h"
#include "chrome/browser/abp/abp_action_context.h"
#include "chrome/browser/abp/abp_console_capture.h"
#include "chrome/browser/abp/abp_input_dispatcher.h"
#include "third_party/re2/src/re2/re2.h"
#include "chrome/browser/abp/abp_popup_interceptor.h"
#include "services/device/public/cpp/geolocation/buildflags.h"
#if BUILDFLAG(OS_LEVEL_GEOLOCATION_PERMISSION_SUPPORTED)
#include "chrome/browser/abp/abp_system_geolocation_source.h"
#endif
#include "base/command_line.h"
#include "chrome/browser/abp/abp_switches.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/synchronization/lock.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_curl_handler.h"
#include "chrome/browser/abp/abp_download_observer.h"
#include "chrome/browser/abp/abp_event_collector.h"
#include "chrome/browser/abp/abp_event_observer.h"
#include "chrome/browser/abp/abp_history_controller.h"
#include "chrome/browser/abp/abp_network_capture.h"
#include "chrome/browser/abp/abp_network_database.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_socket_factory.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "components/input/render_input_router.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "ui/base/cursor/mojom/cursor_type.mojom.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/codec/webp_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "skia/ext/image_operations.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/log/net_log_source.h"
#include "net/socket/tcp_server_socket.h"
#include "ui/snapshot/snapshot.h"
#include "url/gurl.h"

namespace abp {

// CoreAnimation delay after ForceRedraw before GrabViewSnapshot.
// ForceRedraw confirms the compositor committed a frame; this delay gives
// CoreAnimation time to present it on screen. 3 frames @ 16.7ms ≈ 50ms.
constexpr base::TimeDelta kCoreAnimationDelay = base::Milliseconds(50);

// Scale a bitmap down to viewport (DIP) dimensions if it was captured at
// a higher device pixel ratio (e.g. 2x on Retina displays).
static SkBitmap ScaleBitmapToViewport(const SkBitmap& bitmap,
                                      int viewport_width,
                                      int viewport_height) {
  if (viewport_width <= 0 || viewport_height <= 0) {
    return bitmap;
  }
  if (bitmap.width() <= viewport_width && bitmap.height() <= viewport_height) {
    return bitmap;
  }
  return skia::ImageOperations::Resize(
      bitmap, skia::ImageOperations::RESIZE_GOOD,
      viewport_width, viewport_height);
}

// Static instance pointer for test access.
AbpController* AbpController::instance_for_testing_ = nullptr;

// AbpPageLoadObserver implementation
AbpPageLoadObserver::AbpPageLoadObserver(content::WebContents* wc,
                                          LoadCallback callback)
    : content::WebContentsObserver(wc), callback_(std::move(callback)) {}

AbpPageLoadObserver::~AbpPageLoadObserver() = default;

void AbpPageLoadObserver::DOMContentLoaded(
    content::RenderFrameHost* render_frame_host) {
  // Only track main frame events.
  if (render_frame_host->IsInPrimaryMainFrame() && callback_) {
    callback_.Run("dom_content_loaded");
  }
}

void AbpPageLoadObserver::DidFinishLoad(
    content::RenderFrameHost* render_frame_host,
    const GURL& validated_url) {
  // Only track main frame events.
  if (render_frame_host->IsInPrimaryMainFrame() && callback_) {
    callback_.Run("load");
  }
}

void AbpPageLoadObserver::DidFirstVisuallyNonEmptyPaint() {
  if (callback_) {
    callback_.Run("first_paint");
  }
}

// KeyInfo implementation
KeyInfo::KeyInfo() = default;
KeyInfo::KeyInfo(const std::string& k,
                 const std::string& c,
                 int wvk,
                 int nvk,
                 bool is_mod,
                 int mod_flag)
    : key(k),
      code(c),
      windows_virtual_key(wvk),
      native_virtual_key(nvk),
      is_modifier(is_mod),
      modifier_flag(mod_flag) {}
KeyInfo::~KeyInfo() = default;
KeyInfo::KeyInfo(const KeyInfo&) = default;
KeyInfo& KeyInfo::operator=(const KeyInfo&) = default;

// HeldKeyState implementation
AbpController::HeldKeyState::HeldKeyState() = default;
AbpController::HeldKeyState::~HeldKeyState() = default;
AbpController::HeldKeyState::HeldKeyState(const HeldKeyState&) = default;
AbpController::HeldKeyState& AbpController::HeldKeyState::operator=(const HeldKeyState&) = default;

// ActionCompleteWaiter implementation
AbpController::ActionCompleteWaiter::ActionCompleteWaiter() = default;
AbpController::ActionCompleteWaiter::~ActionCompleteWaiter() = default;

// PendingPermissionRequest implementation
AbpController::PendingPermissionRequest::PendingPermissionRequest() = default;
AbpController::PendingPermissionRequest::~PendingPermissionRequest() = default;
AbpController::PendingPermissionRequest::PendingPermissionRequest(
    const PendingPermissionRequest&) = default;
AbpController::PendingPermissionRequest&
AbpController::PendingPermissionRequest::operator=(
    const PendingPermissionRequest&) = default;

// PendingDialog implementation
AbpController::PendingDialog::PendingDialog() = default;
AbpController::PendingDialog::~PendingDialog() = default;
AbpController::PendingDialog::PendingDialog(const PendingDialog&) = default;
AbpController::PendingDialog& AbpController::PendingDialog::operator=(
    const PendingDialog&) = default;

// ActionScreenshotResult / ActionSnapState implementation
AbpController::ActionScreenshotResult::ActionScreenshotResult() = default;
AbpController::ActionScreenshotResult::~ActionScreenshotResult() = default;
AbpController::ActionScreenshotResult::ActionScreenshotResult(
    ActionScreenshotResult&&) = default;
AbpController::ActionScreenshotResult& AbpController::ActionScreenshotResult::
    operator=(ActionScreenshotResult&&) = default;
AbpController::ActionSnapState::ActionSnapState() = default;
AbpController::ActionSnapState::~ActionSnapState() = default;

// ForceRedrawWatcher implementation
AbpController::ForceRedrawWatcher::ForceRedrawWatcher(
    content::RenderWidgetHost* rwh,
    base::WeakPtr<AbpController> controller,
    std::shared_ptr<ActionSnapState> snap_state)
    : rwh_(rwh),
      controller_(std::move(controller)),
      snap_state_(std::move(snap_state)) {
  rwh_->AddObserver(this);
}

AbpController::ForceRedrawWatcher::~ForceRedrawWatcher() {
  Cancel();
}

void AbpController::ForceRedrawWatcher::Cancel() {
  if (cancelled_) {
    return;
  }
  cancelled_ = true;
  if (rwh_) {
    rwh_->RemoveObserver(this);
    rwh_ = nullptr;
  }
  snap_state_.reset();  // Break shared_ptr cycle.
}

void AbpController::ForceRedrawWatcher::RenderWidgetHostDestroyed(
    content::RenderWidgetHost* widget_host) {
  // Must remove from observer list BEFORE any code path that could destroy
  // this watcher (e.g. OnForceRedrawRwhiDestroyed replacing
  // snap_state->watcher). ~RenderWidgetHostObserver DCHECKs !IsInObserverList.
  widget_host->RemoveObserver(this);
  rwh_ = nullptr;
  cancelled_ = true;  // Prevent Cancel() from double-removing.
  if (!snap_state_ || snap_state_->done) {
    return;
  }
  LOG(INFO) << "ABP: ForceRedrawWatcher - RWHI destroyed during in-flight "
            << "ForceRedraw, triggering retry tab=" << snap_state_->tab_id;
  // Steal snap_state before destruction clears it.
  auto stolen_state = std::move(snap_state_);
  if (controller_) {
    controller_->OnForceRedrawRwhiDestroyed(std::move(stolen_state));
  }
}

void AbpController::OnForceRedrawRwhiDestroyed(
    std::shared_ptr<ActionSnapState> snap_state) {
  if (snap_state->done) {
    return;
  }

  content::WebContents* wc = FindWebContents(snap_state->tab_id);
  if (!wc) {
    LOG(WARNING) << "ABP: OnForceRedrawRwhiDestroyed - WebContents gone"
                 << " tab=" << snap_state->tab_id;
    snap_state->done = true;
    std::move(snap_state->cb).Run(ActionScreenshotResult());
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    // New renderer hasn't attached yet — poll at 50ms intervals.
    LOG(INFO) << "ABP: OnForceRedrawRwhiDestroyed - no RWHV yet, polling"
              << " tab=" << snap_state->tab_id;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpController::OnForceRedrawRwhiDestroyed,
                       weak_factory_.GetWeakPtr(), std::move(snap_state)),
        base::Milliseconds(50));
    return;
  }

  // New RWHV is ready — send ForceRedraw on the new RWHI.
  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      view->GetRenderWidgetHost());
  LOG(INFO) << "ABP: OnForceRedrawRwhiDestroyed - retrying ForceRedraw on new "
            << "RWHI tab=" << snap_state->tab_id;

  // Create a new watcher on the new RWHI.
  snap_state->watcher = std::make_shared<ForceRedrawWatcher>(
      rwhi, weak_factory_.GetWeakPtr(), snap_state);
  snap_state->force_redraw_start = base::TimeTicks::Now();

  rwhi->ForceRedrawWithCallback(base::BindOnce(
      [](std::shared_ptr<ActionSnapState> s,
         base::WeakPtr<AbpController> ctrl) {
        if (s->done) return;
        // Cancel the watcher — normal completion.
        if (s->watcher) {
          s->watcher->Cancel();
          s->watcher.reset();
        }
        LOG(INFO) << "ABP: OnForceRedrawRwhiDestroyed - ForceRedraw completed"
                  << " elapsed="
                  << (base::TimeTicks::Now() - s->force_redraw_start)
                         .InMilliseconds()
                  << "ms tab=" << s->tab_id;
        if (!ctrl) {
          s->done = true;
          std::move(s->cb).Run(ActionScreenshotResult());
          return;
        }
        // Wait 50ms for CoreAnimation then grab snapshot.
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(
                &AbpController::GrabViewSnapshotWithFreshnessCheck,
                ctrl, s, /*retry_count=*/0),
            kCoreAnimationDelay);
      },
      snap_state, weak_factory_.GetWeakPtr()));
}

AbpController::TabState::TabState() = default;
AbpController::TabState::~TabState() = default;
AbpController::TabState::TabState(TabState&&) = default;
AbpController::TabState& AbpController::TabState::operator=(TabState&&) = default;

bool AbpController::TabState::IsIdle() const {
  return !cdp_client && !cursor.active && !execution.IsEnabled() &&
         held_keys.held_keys.empty() && !pending_dialog.has_value() &&
         !action_waiter && !action_in_flight &&
         queued_action_starters.empty();
}

void AbpController::TabState::Reset() {
  cdp_client.reset();
  cursor = VirtualCursorState{};
  execution = ExecutionState{};
  held_keys = HeldKeyState{};
  pending_dialog.reset();
  action_waiter.reset();
  action_in_flight = false;
  active_action_epoch = 0;
  next_action_epoch = 0;
  queued_action_starters.clear();
}

AbpController::TabState& AbpController::GetOrCreateTabState(
    const std::string& tab_id) {
  TabState& tab_state = tab_states_[tab_id];
  if (!tab_state.network_capture) {
    tab_state.network_capture = std::make_unique<AbpNetworkCapture>();
  }
  return tab_state;
}

void AbpController::CleanupTabState(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) {
    return;
  }

  // Remove console observer before erasing tab state.
  console_observers_.erase(tab_id);

  // Drain queued actions — each AbpActionContext will discover the tab
  // is gone during Start() and send a 404 error response to the client.
  auto queued = std::move(it->second.queued_action_starters);
  tab_states_.erase(it);

  for (auto& starter : queued) {
    // Epoch 0 is a sentinel — IsDeterministicActionCurrent will return false
    // since the tab state no longer exists, but the AbpActionContext::Start()
    // will run far enough to find the tab missing and send an error response.
    std::move(starter).Run(0);
  }
}

bool AbpController::RunOrQueueDeterministicAction(
    const std::string& tab_id,
    base::OnceCallback<void(uint64_t)> starter) {
  TabState& state = GetOrCreateTabState(tab_id);
  if (state.action_in_flight) {
    if (state.queued_action_starters.size() >= kMaxQueuedActionsPerTab) {
      LOG(WARNING) << "ABP: Action queue full for tab " << tab_id
                   << " (" << kMaxQueuedActionsPerTab << " queued)";
      return false;
    }
    state.queued_action_starters.push_back(std::move(starter));
    return true;
  }

  state.action_in_flight = true;
  state.active_action_epoch = ++state.next_action_epoch;
  std::move(starter).Run(state.active_action_epoch);
  return true;
}

void AbpController::FinishDeterministicAction(const std::string& tab_id,
                                              uint64_t action_epoch) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) {
    return;
  }

  TabState& state = it->second;
  if (!state.action_in_flight || state.active_action_epoch != action_epoch) {
    return;
  }

  state.action_in_flight = false;

  if (!state.queued_action_starters.empty()) {
    auto starter = std::move(state.queued_action_starters.front());
    state.queued_action_starters.pop_front();
    state.action_in_flight = true;
    state.active_action_epoch = ++state.next_action_epoch;
    std::move(starter).Run(state.active_action_epoch);
  }
}

bool AbpController::IsDeterministicActionCurrent(const std::string& tab_id,
                                                 uint64_t action_epoch) const {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) {
    return false;
  }
  const TabState& state = it->second;
  return state.action_in_flight && state.active_action_epoch == action_epoch;
}

namespace {

// Key code mapping table for CDP Input.dispatchKeyEvent
// See: https://chromedevtools.github.io/devtools-protocol/tot/Input/#method-dispatchKeyEvent
struct KeyMapping {
  const char* name;
  const char* key;
  const char* code;
  const char* text;  // Text generated by keypress (e.g., "\r" for Enter)
  int vk;
  bool is_modifier;
  int modifier_flag;  // 1=Alt, 2=Ctrl, 4=Meta, 8=Shift
};

// clang-format off
constexpr KeyMapping kKeyMappings[] = {
    //  name             key          code              text   vk  mod?  flag
    // Modifiers
    {"Alt",          "Alt",       "AltLeft",        "",    18, true,  1},
    {"AltLeft",      "Alt",       "AltLeft",        "",    18, true,  1},
    {"AltRight",     "Alt",       "AltRight",       "",    18, true,  1},
    {"Control",      "Control",   "ControlLeft",    "",    17, true,  2},
    {"ControlLeft",  "Control",   "ControlLeft",    "",    17, true,  2},
    {"ControlRight", "Control",   "ControlRight",   "",    17, true,  2},
    {"Meta",         "Meta",      "MetaLeft",       "",    91, true,  4},
    {"MetaLeft",     "Meta",      "MetaLeft",       "",    91, true,  4},
    {"MetaRight",    "Meta",      "MetaRight",      "",    92, true,  4},
    {"Shift",        "Shift",     "ShiftLeft",      "",    16, true,  8},
    {"ShiftLeft",    "Shift",     "ShiftLeft",      "",    16, true,  8},
    {"ShiftRight",   "Shift",     "ShiftRight",     "",    16, true,  8},

    // Special keys — Enter and Tab generate text events
    {"Enter",     "Enter",     "Enter",     "\r",  13, false, 0},
    {"Tab",       "Tab",       "Tab",       "\t",   9, false, 0},
    {"Escape",    "Escape",    "Escape",    "",    27, false, 0},
    {"Backspace", "Backspace", "Backspace", "",     8, false, 0},
    {"Delete",    "Delete",    "Delete",    "",    46, false, 0},
    {"Insert",    "Insert",    "Insert",    "",    45, false, 0},
    {"Home",      "Home",      "Home",      "",    36, false, 0},
    {"End",       "End",       "End",       "",    35, false, 0},
    {"PageUp",    "PageUp",    "PageUp",    "",    33, false, 0},
    {"PageDown",  "PageDown",  "PageDown",  "",    34, false, 0},
    {"Space",     " ",         "Space",     "",    32, false, 0},
    {" ",         " ",         "Space",     "",    32, false, 0},

    // Arrow keys
    {"ArrowUp",    "ArrowUp",    "ArrowUp",    "", 38, false, 0},
    {"ArrowDown",  "ArrowDown",  "ArrowDown",  "", 40, false, 0},
    {"ArrowLeft",  "ArrowLeft",  "ArrowLeft",  "", 37, false, 0},
    {"ArrowRight", "ArrowRight", "ArrowRight", "", 39, false, 0},

    // Function keys
    {"F1",  "F1",  "F1",  "", 112, false, 0},
    {"F2",  "F2",  "F2",  "", 113, false, 0},
    {"F3",  "F3",  "F3",  "", 114, false, 0},
    {"F4",  "F4",  "F4",  "", 115, false, 0},
    {"F5",  "F5",  "F5",  "", 116, false, 0},
    {"F6",  "F6",  "F6",  "", 117, false, 0},
    {"F7",  "F7",  "F7",  "", 118, false, 0},
    {"F8",  "F8",  "F8",  "", 119, false, 0},
    {"F9",  "F9",  "F9",  "", 120, false, 0},
    {"F10", "F10", "F10", "", 121, false, 0},
    {"F11", "F11", "F11", "", 122, false, 0},
    {"F12", "F12", "F12", "", 123, false, 0},

    // Symbol keys (DOM key values)
    {"Comma",        ",",  "Comma",        "", 188, false, 0},
    {"Period",       ".",  "Period",       "", 190, false, 0},
    {"Slash",        "/",  "Slash",        "", 191, false, 0},
    {"Backslash",    "\\", "Backslash",    "", 220, false, 0},
    {"Semicolon",    ";",  "Semicolon",    "", 186, false, 0},
    {"Quote",        "'",  "Quote",        "", 222, false, 0},
    {"BracketLeft",  "[",  "BracketLeft",  "", 219, false, 0},
    {"BracketRight", "]",  "BracketRight", "", 221, false, 0},
    {"Minus",        "-",  "Minus",        "", 189, false, 0},
    {"Equal",        "=",  "Equal",        "", 187, false, 0},
    {"Backquote",    "`",  "Backquote",    "", 192, false, 0},

    // Number row
    {"0", "0", "Digit0", "", 48, false, 0},
    {"1", "1", "Digit1", "", 49, false, 0},
    {"2", "2", "Digit2", "", 50, false, 0},
    {"3", "3", "Digit3", "", 51, false, 0},
    {"4", "4", "Digit4", "", 52, false, 0},
    {"5", "5", "Digit5", "", 53, false, 0},
    {"6", "6", "Digit6", "", 54, false, 0},
    {"7", "7", "Digit7", "", 55, false, 0},
    {"8", "8", "Digit8", "", 56, false, 0},
    {"9", "9", "Digit9", "", 57, false, 0},
};
// clang-format on

}  // namespace

// Key info helper - defined outside anonymous namespace so it's accessible
KeyInfo GetKeyInfo(const std::string& key_name) {
  // First check the mapping table (case-insensitive to accept ALL-CAPS from
  // NormalizeKey as well as MixedCase CDP names).
  for (const auto& mapping : kKeyMappings) {
    if (base::EqualsCaseInsensitiveASCII(key_name, mapping.name)) {
      KeyInfo info;
      info.key = mapping.key;
      info.code = mapping.code;
      info.text = mapping.text;
      info.windows_virtual_key = mapping.vk;
      info.native_virtual_key = mapping.vk;
      info.is_modifier = mapping.is_modifier;
      info.modifier_flag = mapping.modifier_flag;
      return info;
    }
  }

  // Handle single lowercase letters (a-z)
  if (key_name.length() == 1) {
    char c = key_name[0];
    if (c >= 'a' && c <= 'z') {
      std::string code = std::string("Key") + static_cast<char>(std::toupper(c));
      int vk = std::toupper(c);  // VK codes for letters are uppercase ASCII
      return {key_name, code, vk, vk, false, 0};
    }
    // Handle uppercase letters
    if (c >= 'A' && c <= 'Z') {
      std::string code = std::string("Key") + c;
      int vk = c;
      return {key_name, code, vk, vk, false, 0};
    }
  }

  // Fallback: use the key name as-is
  LOG(WARNING) << "ABP: Unknown key name: " << key_name << ", using as-is";
  return {key_name, key_name, 0, 0, false, 0};
}

int ModifiersToFlags(const std::vector<std::string>& modifiers) {
  int flags = 0;
  for (const auto& mod : modifiers) {
    std::string m = base::ToUpperASCII(mod);
    if (m == "ALT" || m == "ALTLEFT" || m == "ALTRIGHT") {
      flags |= 1;
    } else if (m == "CONTROL" || m == "CONTROLLEFT" || m == "CONTROLRIGHT") {
      flags |= 2;
    } else if (m == "META" || m == "METALEFT" || m == "METARIGHT") {
      flags |= 4;
    } else if (m == "SHIFT" || m == "SHIFTLEFT" || m == "SHIFTRIGHT") {
      flags |= 8;
    }
  }
  return flags;
}

std::optional<std::string> NormalizeKey(const std::string& input) {
  // Static abbreviation map — built once, never destroyed.
  static const base::NoDestructor<base::flat_map<std::string, std::string>>
      kAbbreviations(base::flat_map<std::string, std::string>({
          // Modifier abbreviations
          {"CTRL", "CONTROL"},
          {"\xe2\x8c\x83", "CONTROL"},   // ⌃ (U+2303)
          {"CMD", "META"},
          {"COMMAND", "META"},
          {"\xe2\x8c\x98", "META"},      // ⌘ (U+2318)
          {"OPT", "ALT"},
          {"OPTION", "ALT"},
          {"\xe2\x8c\xa5", "ALT"},       // ⌥ (U+2325)
          {"\xe2\x87\xa7", "SHIFT"},     // ⇧ (U+21E7)
          // Special key abbreviations
          {"ESC", "ESCAPE"},
          {"DEL", "DELETE"},
          {"BS", "BACKSPACE"},
          {"CR", "ENTER"},
          {"RETURN", "ENTER"},
          {"INS", "INSERT"},
          {"PGUP", "PAGEUP"},
          {"PGDN", "PAGEDOWN"},
          {"PGDOWN", "PAGEDOWN"},
          // Arrow key abbreviations
          {"UP", "ARROWUP"},
          {"DOWN", "ARROWDOWN"},
          {"LEFT", "ARROWLEFT"},
          {"RIGHT", "ARROWRIGHT"},
      }));

  // Static valid key set — built once, never destroyed.
  static const base::NoDestructor<base::flat_set<std::string>> kValidKeys([] {
    std::vector<std::string> keys;
    // Letters A-Z
    for (char c = 'A'; c <= 'Z'; ++c) {
      keys.emplace_back(1, c);
    }
    // Digits 0-9
    for (char c = '0'; c <= '9'; ++c) {
      keys.emplace_back(1, c);
    }
    // Function keys F1-F12 (kKeyMappings only defines F1-F12)
    for (int i = 1; i <= 12; ++i) {
      keys.push_back("F" + base::NumberToString(i));
    }
    // Navigation keys
    for (const char* k : {"ARROWUP", "ARROWDOWN", "ARROWLEFT", "ARROWRIGHT",
                           "HOME", "END", "PAGEUP", "PAGEDOWN"}) {
      keys.push_back(k);
    }
    // Editing keys
    for (const char* k : {"BACKSPACE", "DELETE", "INSERT", "ENTER", "TAB",
                           "ESCAPE", "SPACE"}) {
      keys.push_back(k);
    }
    // Modifier keys
    for (const char* k : {"SHIFT", "CONTROL", "ALT", "META"}) {
      keys.push_back(k);
    }
    // Symbol keys (mapped from DOM key values)
    for (const char* k : {"COMMA", "PERIOD", "SLASH", "BACKSLASH",
                           "SEMICOLON", "QUOTE", "BRACKETLEFT",
                           "BRACKETRIGHT", "MINUS", "EQUAL", "BACKQUOTE"}) {
      keys.push_back(k);
    }
    return base::flat_set<std::string>(std::move(keys));
  }());

  if (input.empty()) {
    return std::nullopt;
  }

  // Uppercase the input for case-insensitive matching.
  std::string upper = base::ToUpperASCII(input);

  // Check abbreviation map with the uppercased string first.
  auto it = kAbbreviations->find(upper);
  if (it != kAbbreviations->end()) {
    return it->second;
  }
  // Also check with the original input (for Unicode symbols like ⌘ that
  // don't change under ASCII uppercasing).
  it = kAbbreviations->find(input);
  if (it != kAbbreviations->end()) {
    return it->second;
  }

  // Validate against the known key set.
  if (kValidKeys->contains(upper)) {
    return upper;
  }

  return std::nullopt;
}

// Thread-safe storage for the actual bound address from the CDP server's
// handler thread, readable by the UI thread. Defined in abp namespace
// (not anonymous) so the header's forward declaration resolves correctly.
struct CdpBoundEndpoint {
  base::Lock lock;
  std::string address;  // e.g. "127.0.0.1" or "::1"
  int port = 0;

  void Set(const std::string& addr, int p) {
    base::AutoLock l(lock);
    address = addr;
    port = p;
  }

  std::string GetAddress() {
    base::AutoLock l(lock);
    return address;
  }

  int GetPort() {
    base::AutoLock l(lock);
    return port;
  }
};

namespace {

// Socket factory for CDP remote debugging server.
// Creates a TCP server socket bound to localhost on the specified port.
// Writes the actual bound address to shared CdpBoundEndpoint so the UI
// thread can construct accurate WebSocket URLs.
class AbpCdpSocketFactory : public content::DevToolsSocketFactory {
 public:
  AbpCdpSocketFactory(int port,
                      std::shared_ptr<CdpBoundEndpoint> bound_endpoint)
      : port_(port), bound_endpoint_(std::move(bound_endpoint)) {}

  std::unique_ptr<net::ServerSocket> CreateForHttpServer() override {
    auto socket = std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    net::IPEndPoint endpoint(net::IPAddress::IPv4Localhost(), port_);
    if (socket->Listen(endpoint, 10, std::nullopt) == net::OK) {
      net::IPEndPoint local_addr;
      socket->GetLocalAddress(&local_addr);
      bound_endpoint_->Set(local_addr.address().ToString(), local_addr.port());
      return socket;
    }
    socket = std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    net::IPEndPoint endpoint6(net::IPAddress::IPv6Localhost(), port_);
    if (socket->Listen(endpoint6, 10, std::nullopt) == net::OK) {
      net::IPEndPoint local_addr;
      socket->GetLocalAddress(&local_addr);
      bound_endpoint_->Set(local_addr.address().ToString(), local_addr.port());
      return socket;
    }
    return nullptr;
  }

  std::unique_ptr<net::ServerSocket> CreateForTethering(
      std::string* out_name) override {
    return nullptr;
  }

 private:
  int port_;
  std::shared_ptr<CdpBoundEndpoint> bound_endpoint_;
};

}  // namespace

namespace {

bool IsValidMarkupTag(const std::string& tag) {
  return tag == "clickable" || tag == "typeable" ||
         tag == "scrollable" || tag == "grid" || tag == "selected";
}

}  // namespace

// static
bool AbpController::ValidateMarkupTags(const std::vector<std::string>& tags,
                                       std::string* invalid_tag) {
  for (const auto& tag : tags) {
    if (!IsValidMarkupTag(tag)) {
      *invalid_tag = tag;
      return false;
    }
  }
  return true;
}

namespace {

// Parse path like "/api/v1/tabs/ABC123/navigate" into segments
std::vector<std::string> ParsePath(const std::string& path) {
  std::vector<std::string> segments;
  for (const auto& segment : base::SplitString(
           path, "/", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    segments.push_back(segment);
  }
  return segments;
}

}  // namespace

// AbpCdpClient implementation

AbpCdpClient::AbpCdpClient(scoped_refptr<content::DevToolsAgentHost> host)
    : host_(std::move(host)) {
  host_->AttachClient(this);
}

AbpCdpClient::~AbpCdpClient() {
  if (host_) {
    host_->DetachClient(this);
  }
}

void AbpCdpClient::SendCommand(const std::string& method,
                               const base::Value::Dict& params,
                               CdpCallback callback) {
  int command_id = next_command_id_++;
  pending_callbacks_[command_id] = std::move(callback);

  // Build CDP message: {"id": N, "method": "...", "params": {...}}
  base::Value::Dict message;
  message.Set("id", command_id);
  message.Set("method", method);
  message.Set("params", params.Clone());

  std::string json;
  base::JSONWriter::Write(message, &json);

  // Send to DevTools agent
  host_->DispatchProtocolMessage(this, base::as_byte_span(json));
}

void AbpCdpClient::SetEventListener(EventCallback callback) {
  event_listener_ = std::move(callback);
}

void AbpCdpClient::ClearEventListener() {
  event_listener_.Reset();
}

void AbpCdpClient::DispatchProtocolMessage(content::DevToolsAgentHost* host,
                                           base::span<const uint8_t> message) {
  // Parse the CDP response
  std::string json(message.begin(), message.end());
  auto parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return;
  }

  const base::Value::Dict& response = parsed->GetDict();

  // Check for command response (has "id")
  auto id = response.FindInt("id");
  if (!id) {
    // This is an event notification
    const std::string* method = response.FindString("method");
    if (method && event_listener_) {
      const base::Value::Dict* params = response.FindDict("params");
      if (params) {
        event_listener_.Run(*method, *params);
      } else {
        base::Value::Dict empty_params;
        event_listener_.Run(*method, empty_params);
      }
    }
    return;
  }

  auto it = pending_callbacks_.find(*id);
  if (it == pending_callbacks_.end()) {
    return;
  }

  CdpCallback callback = std::move(it->second);
  pending_callbacks_.erase(it);

  // Check for error
  const base::Value::Dict* error = response.FindDict("error");
  if (error) {
    const std::string* error_msg = error->FindString("message");
    std::move(callback).Run(false, error_msg ? *error_msg : "Unknown error");
    return;
  }

  // Get result
  const base::Value* result = response.Find("result");
  if (result) {
    std::string result_json;
    base::JSONWriter::Write(*result, &result_json);
    std::move(callback).Run(true, result_json);
  } else {
    std::move(callback).Run(true, "{}");
  }
}

void AbpCdpClient::AgentHostClosed(content::DevToolsAgentHost* host) {
  host_ = nullptr;
  // Fail all pending callbacks
  for (auto& [id, callback] : pending_callbacks_) {
    std::move(callback).Run(false, "Agent host closed");
  }
  pending_callbacks_.clear();
}

// ScreenshotOptions implementation

AbpController::ScreenshotOptions::ScreenshotOptions() = default;
AbpController::ScreenshotOptions::~ScreenshotOptions() = default;
AbpController::ScreenshotOptions::ScreenshotOptions(const ScreenshotOptions&) = default;
AbpController::ScreenshotOptions& AbpController::ScreenshotOptions::operator=(
    const ScreenshotOptions&) = default;

// AbpController implementation

AbpController::AbpController()
    : event_collector_(std::make_unique<AbpEventCollector>(this)),
      input_dispatcher_(std::make_unique<AbpInputDispatcher>(this)),
      popup_interceptor_(std::make_unique<AbpPopupInterceptor>(this)),
      permission_observer_(std::make_unique<AbpPermissionObserver>(this)),
      console_capture_(std::make_unique<AbpConsoleCapture>()) {
  instance_for_testing_ = this;
}

AbpController::~AbpController() {
  for (TabStripModel* ts : observed_tab_strips_) {
    ts->RemoveObserver(this);
  }
  observed_tab_strips_.clear();

  if (instance_for_testing_ == this) {
    instance_for_testing_ = nullptr;
  }
}

void AbpController::SetLifecycleObserverForTesting(
    LifecycleObserverCallback cb) {
  lifecycle_observer_for_testing_ = std::move(cb);
}

// static
AbpController* AbpController::GetInstance() {
  return instance_for_testing_;
}

// static
AbpController* AbpController::GetInstanceForTesting() {
  return instance_for_testing_;
}

// static
int AbpController::GetForceRedrawQueuedCountForTesting() {
  return content::RenderWidgetHostImpl::force_redraw_queued_count_for_testing();
}

// static
void AbpController::ResetForceRedrawCountersForTesting() {
  content::RenderWidgetHostImpl::ResetForceRedrawCountersForTesting();
}

void AbpController::SetHistoryController(
    AbpHistoryController* history_controller) {
  history_controller_ = history_controller;
}

std::string AbpController::GetActiveTabId() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Get the first browser with an active tab
  const BrowserList* browser_list = BrowserList::GetInstance();
  for (auto it = browser_list->begin(); it != browser_list->end(); ++it) {
    Browser* browser = *it;
    if (browser->tab_strip_model()->count() > 0) {
      content::WebContents* wc =
          browser->tab_strip_model()->GetActiveWebContents();
      if (wc) {
        // Use GetOrCreateFor() to match FindWebContents() which also uses it.
        // Note: GetOrCreateForTab() returns a 'tab' target with a different ID.
        scoped_refptr<content::DevToolsAgentHost> host =
            content::DevToolsAgentHost::GetOrCreateFor(wc);
        if (host) {
          return host->GetId();
        }
      }
    }
  }

  return std::string();
}

std::string AbpController::GetTabIdForWebContents(content::WebContents* wc) {
  if (!wc)
    return std::string();

  scoped_refptr<content::DevToolsAgentHost> host =
      content::DevToolsAgentHost::GetOrCreateFor(wc);
  if (host) {
    return host->GetId();
  }
  return std::string();
}

void AbpController::EmitPopupEvent(const std::string& event_type,
                                    base::Value::Dict event_data) {
  if (event_collector_) {
    event_collector_->AddEvent(event_type, std::move(event_data));
  }
}

void AbpController::OnHttpAuthDismissed(const std::string& tab_id,
                                         base::Value::Dict event_data) {
  // RecordEvent takes const ref, so call it first. Then move to AddEvent.
  if (history_controller_) {
    history_controller_->RecordEvent(tab_id, "http_auth_dismissed", event_data);
  }
  if (event_collector_) {
    event_collector_->AddEvent("http_auth_dismissed", std::move(event_data));
  }
}

void AbpController::CenterCursorInTab(const std::string& tab_id,
                                       base::OnceClosure callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  VLOG(1) << "ABP DEBUG L1: CenterCursorInTab called for tab " << tab_id;

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    VLOG(1) << "ABP DEBUG L1: CenterCursorInTab - WebContents null for tab " << tab_id;
    std::move(callback).Run();
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    VLOG(1) << "ABP DEBUG L1: CenterCursorInTab - RWHV null for tab " << tab_id;
    std::move(callback).Run();
    return;
  }

  // Get viewport size.
  gfx::Size viewport_size = rwhv->GetVisibleViewportSize();
  double center_x = viewport_size.width() / 2.0;
  double center_y = viewport_size.height() / 2.0;

  VLOG(1) << "ABP DEBUG L1: CenterCursorInTab"
            << " tab=" << tab_id
            << " viewport=" << viewport_size.width() << "x" << viewport_size.height()
            << " center=(" << center_x << ", " << center_y << ")";

  // Update internal virtual cursor state.
  UpdateVirtualCursorState(tab_id, center_x, center_y);

  // Enable and set virtual cursor via Mojo for on-screen rendering.
  // TEMPORARY: Check if RenderWidgetHost is ready before calling Mojo methods
  content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
  if (rwh && rwh->GetProcess() && rwh->GetProcess()->IsInitializedAndNotDead()) {
    VLOG(1) << "ABP DEBUG L1: CenterCursorInTab - calling SetVirtualCursorEnabledViaMojo(true)";
    SetVirtualCursorEnabledViaMojo(wc, true);
    VLOG(1) << "ABP DEBUG L1: CenterCursorInTab - calling SetVirtualCursorViaMojo(" << center_x << ", " << center_y << ", true)";
    SetVirtualCursorViaMojo(wc, center_x, center_y, true);
  } else {
    VLOG(1) << "ABP DEBUG L1: CenterCursorInTab - skipping Mojo calls, RWH not ready";
  }

  InsertVisualStateFence(
      tab_id,
      base::BindOnce(
          [](base::OnceClosure cb, bool ready) {
            if (!ready) {
              VLOG(1) << "ABP: CenterCursorInTab visual-state fence failed";
            }
            std::move(cb).Run();
          },
          std::move(callback)));
}

bool AbpController::IsBrowserReady() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  const BrowserList* browser_list = BrowserList::GetInstance();
  for (auto it = browser_list->begin(); it != browser_list->end(); ++it) {
    Browser* browser = *it;
    if (browser->tab_strip_model()->count() > 0) {
      content::WebContents* wc =
          browser->tab_strip_model()->GetActiveWebContents();
      content::RenderWidgetHostView* rwhv = wc ? wc->GetRenderWidgetHostView() : nullptr;
      VLOG(1) << "ABP DEBUG L1: IsBrowserReady check"
                << " wc=" << (wc ? "valid" : "null")
                << " rwhv=" << (rwhv ? "valid" : "null");
      if (rwhv) {
        return true;
      }
    }
  }
  VLOG(1) << "ABP DEBUG L1: IsBrowserReady - not ready yet";
  return false;
}

void AbpController::GetBrowserStatus(ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  bool has_browser_window = false;
  bool has_devtools = false;

  const BrowserList* browser_list = BrowserList::GetInstance();
  for (auto it = browser_list->begin(); it != browser_list->end(); ++it) {
    Browser* browser = *it;
    if (browser->tab_strip_model()->count() > 0) {
      has_browser_window = true;
      content::WebContents* wc =
          browser->tab_strip_model()->GetActiveWebContents();
      if (wc) {
        // Check if we can create/get a CDP client (indicates DevTools ready)
        if (input_mode_ != InputMode::kCdp) {
          AbpCdpClient* client = GetOrCreateCdpClient(wc);
          if (client) {
            has_devtools = true;
          }
        } else {
          has_devtools = true;
        }
      }
      break;
    }
  }

  bool ready = has_browser_window && has_devtools;

  base::Value::Dict components;
  components.Set("http_server", true);  // Always true if handling request
  components.Set("browser_window", has_browser_window);
  components.Set("devtools", has_devtools);

  base::Value::Dict data;
  data.Set("ready", ready);
  data.Set("state", ready ? "ready" : "initializing");
  {
    const char* mode_str = "agent";
    if (input_mode_ == InputMode::kHuman) mode_str = "human";
    else if (input_mode_ == InputMode::kCdp) mode_str = "cdp";
    data.Set("input_mode", mode_str);
  }
  data.Set("components", std::move(components));

  if (input_mode_ == InputMode::kCdp && cdp_port_ > 0) {
    base::Value::Dict cdp;
    // Read actual bound address from shared endpoint (set by handler thread).
    // Falls back to the pre-test address if the server hasn't bound yet.
    std::string addr = cdp_bound_address_;
    int port = cdp_port_;
    if (cdp_bound_endpoint_) {
      std::string actual_addr = cdp_bound_endpoint_->GetAddress();
      int actual_port = cdp_bound_endpoint_->GetPort();
      if (!actual_addr.empty()) {
        addr = actual_addr;
        port = actual_port;
        // Format IPv6 with brackets for URL.
        std::string url_host = addr.find(':') != std::string::npos
            ? "[" + addr + "]" : addr;
        cdp_ws_url_ = "ws://" + url_host + ":" + std::to_string(port) + "/devtools/browser";
      }
    }
    cdp.Set("port", port);
    cdp.Set("address", addr);
    cdp.Set("ws_url", cdp_ws_url_);
    if (!cdp_timeout_deadline_.is_null()) {
      int remaining_ms = static_cast<int>(
          (cdp_timeout_deadline_ - base::TimeTicks::Now()).InMilliseconds());
      cdp.Set("remaining_ms", std::max(0, remaining_ms));
    }
    data.Set("cdp", std::move(cdp));
  }

  if (!ready) {
    if (!has_browser_window) {
      data.Set("message", "Waiting for browser window");
    } else if (!has_devtools) {
      data.Set("message", "Waiting for DevTools connection");
    }
  }

  base::Value::Dict response;
  response.Set("success", true);
  response.Set("data", std::move(data));

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::GetSessionData(ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  base::Value::Dict data;

  if (history_controller_) {
    // Session dir is already resolved to an absolute path during config loading.
    const base::FilePath& session_dir = history_controller_->SessionDir();
    data.Set("session_dir", session_dir.AsUTF8Unsafe());
    data.Set("database_path",
             session_dir
                 .Append(history_controller_->DatabasePath().BaseName())
                 .AsUTF8Unsafe());
    data.Set("screenshots_dir",
             session_dir
                 .Append(
                     history_controller_->ScreenshotsDirectory().BaseName())
                 .AsUTF8Unsafe());
    data.Set("screenshots_enabled", history_controller_->ScreenshotsEnabled());
  } else {
    data.Set("session_dir", base::Value());
    data.Set("database_path", base::Value());
    data.Set("screenshots_dir", base::Value());
    data.Set("screenshots_enabled", false);
  }

  base::Value::Dict response;
  response.Set("success", true);
  response.Set("data", std::move(data));

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::CaptureScreenshotForHistory(
    const std::string& tab_id,
    int64_t timestamp,
    bool is_before,
    base::OnceCallback<void(std::string path)> callback) {
  VLOG(1) << "ABP: CaptureScreenshotForHistory tab=" << tab_id
            << " is_before=" << is_before
            << " history_controller=" << (history_controller_ ? "yes" : "no")
            << " screenshots_enabled="
            << (history_controller_ ? (history_controller_->ScreenshotsEnabled() ? "yes" : "no") : "n/a");
  if (!history_controller_ || !history_controller_->ScreenshotsEnabled()) {
    VLOG(1) << "ABP: CaptureScreenshotForHistory - skipping (disabled)";
    std::move(callback).Run("");
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run("");
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    LOG(WARNING) << "ABP: CaptureScreenshotForHistory - no RenderWidgetHostView";
    std::move(callback).Run("");
    return;
  }

  base::FilePath screenshot_path =
      history_controller_->GetScreenshotPath(tab_id, timestamp, is_before);

  // Use ForceRedrawWithCallback + GrabViewSnapshot instead of CDP
  // Page.captureScreenshot which can hang indefinitely.
  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      view->GetRenderWidgetHost());

  struct HistorySnapState {
    bool done = false;
    base::OnceCallback<void(std::string)> cb;
    base::FilePath path;
    std::string tab_id;
  };
  auto st = std::make_shared<HistorySnapState>();
  st->cb = std::move(callback);
  st->path = screenshot_path;
  st->tab_id = tab_id;

  // 1500ms safety timeout
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<HistorySnapState> s) {
            if (s->done) return;
            s->done = true;
            LOG(WARNING) << "ABP: CaptureScreenshotForHistory timed out";
            std::move(s->cb).Run("");
          },
          st),
      base::Milliseconds(1500));

  VLOG(1) << "ABP: CaptureScreenshotForHistory - calling ForceRedrawWithCallback";
  rwhi->ForceRedrawWithCallback(base::BindOnce(
      [](std::shared_ptr<HistorySnapState> s,
         base::WeakPtr<AbpController> ctrl) {
        if (s->done) return;
        VLOG(1) << "ABP: CaptureScreenshotForHistory - ForceRedraw callback fired";
        if (!ctrl) {
          s->done = true;
          std::move(s->cb).Run("");
          return;
        }
        // 50ms CoreAnimation delay for compositor to present frame.
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(
                [](std::shared_ptr<HistorySnapState> s,
                   base::WeakPtr<AbpController> ctrl) {
                  if (s->done) return;
                  if (!ctrl) {
                    s->done = true;
                    std::move(s->cb).Run("");
                    return;
                  }
                  content::WebContents* wc2 =
                      ctrl->FindWebContents(s->tab_id);
                  if (!wc2) {
                    s->done = true;
                    std::move(s->cb).Run("");
                    return;
                  }
                  gfx::NativeView native_view = wc2->GetContentNativeView();
                  if (!native_view) {
                    s->done = true;
                    std::move(s->cb).Run("");
                    return;
                  }
                  auto* view2 = wc2->GetRenderWidgetHostView();
                  gfx::Rect bounds;
                  if (view2) {
                    bounds = gfx::Rect(view2->GetViewBounds().size());
                  }
                  ui::GrabViewSnapshot(
                      native_view, bounds,
                      base::BindOnce(
                          [](std::shared_ptr<HistorySnapState> s,
                             gfx::Image image) {
                            if (s->done) return;
                            s->done = true;
                            if (image.IsEmpty()) {
                              LOG(WARNING) << "ABP: CaptureScreenshotForHistory"
                                           << " - GrabViewSnapshot empty";
                              std::move(s->cb).Run("");
                              return;
                            }
                            VLOG(1) << "ABP: CaptureScreenshotForHistory"
                                      << " - encoding WebP";
                            const SkBitmap& bitmap = *image.ToSkBitmap();
                            auto encoded =
                                gfx::WebpCodec::Encode(bitmap, 80);
                            if (!encoded || encoded->empty()) {
                              LOG(WARNING)
                                  << "ABP: CaptureScreenshotForHistory"
                                  << " - WebP encode failed";
                              std::move(s->cb).Run("");
                              return;
                            }
                            VLOG(1) << "ABP: CaptureScreenshotForHistory"
                                      << " - writing file "
                                      << s->path.value();
                            base::ThreadPool::PostTaskAndReplyWithResult(
                                FROM_HERE, {base::MayBlock()},
                                base::BindOnce(
                                    [](base::FilePath p,
                                       std::vector<uint8_t> content)
                                        -> std::string {
                                      if (base::WriteFile(p, content)) {
                                        return p.AsUTF8Unsafe();
                                      }
                                      return "";
                                    },
                                    s->path, std::move(*encoded)),
                                std::move(s->cb));
                          },
                          s));
                },
                s, ctrl),
            kCoreAnimationDelay);
      },
      st, weak_factory_.GetWeakPtr()));
}

// static
std::string AbpController::BuildMarkupInjectionScript(
    const std::vector<std::string>& markup_tags) {
  // Build CSS rules for pure-CSS tags
  std::string css_rules;
  for (const auto& tag : markup_tags) {
    if (tag == "clickable") {
      css_rules += R"(
        a,[role='link']{outline:2px solid #4CAF50!important;outline-offset:-2px!important}
        button,[role='button'],[onclick],[tabindex]:not([tabindex='-1']){outline:2px solid #4CAF50!important;outline-offset:-2px!important}
      )";
    } else if (tag == "typeable") {
      css_rules += R"(
        input:not([type='hidden']):not([type='checkbox']):not([type='radio']):not([type='submit']):not([type='button']),
        textarea,[contenteditable='true']{outline:2px solid #FF9800!important;outline-offset:-2px!important}
      )";
    } else if (tag == "scrollable") {
      css_rules += R"(
        .abp-scrollable{outline:2px dashed #9C27B0!important;outline-offset:-2px!important}
      )";
    } else if (tag == "selected") {
      css_rules += R"(
        *:focus{outline:3px solid #2196F3!important;outline-offset:-3px!important}
      )";
    }
  }

  // Check which JS-assisted tags are active
  bool has_scrollable = std::find(markup_tags.begin(), markup_tags.end(),
                                  "scrollable") != markup_tags.end();
  bool has_grid = std::find(markup_tags.begin(), markup_tags.end(),
                            "grid") != markup_tags.end();

  std::string script = "(function(){";

  // Remove any existing overlay
  script += "var old=document.getElementById('abp-markup-overlay');";
  script += "if(old)old.remove();";

  // Create wrapper div
  script += "var w=document.createElement('div');";
  script += "w.id='abp-markup-overlay';";

  // Add style element with CSS rules
  if (!css_rules.empty()) {
    script += "var s=document.createElement('style');";
    script += "s.textContent=`" + css_rules + "`;";
    script += "w.appendChild(s);";
  }

  // Scrollable: walk DOM and add classes
  if (has_scrollable) {
    script += R"(
      document.querySelectorAll('*').forEach(function(el){
        if(el.tagName==='BODY'||el.tagName==='HTML')return;
        var cs=getComputedStyle(el);
        var ov=cs.overflow+cs.overflowX+cs.overflowY;
        if(!/auto|scroll/.test(ov))return;
        if(el.scrollHeight>el.clientHeight||el.scrollWidth>el.clientWidth){
          el.classList.add('abp-scrollable');
        }
      });
    )";
  }

  // Grid: create overlay div with lines and labels
  if (has_grid) {
    script += R"(
      var g=document.createElement('div');
      g.id='abp-markup-grid';
      g.style.cssText='position:fixed;inset:0;z-index:2147483647;pointer-events:none;'+
        'background:repeating-linear-gradient(to right,rgba(255,0,0,0.3) 0px,rgba(255,0,0,0.3) 1px,transparent 1px,transparent 100px),'+
        'repeating-linear-gradient(to bottom,rgba(255,0,0,0.3) 0px,rgba(255,0,0,0.3) 1px,transparent 1px,transparent 100px)';
      var vw=document.documentElement.clientWidth;
      var vh=document.documentElement.clientHeight;
      for(var x=100;x<vw;x+=100){
        var lbl=document.createElement('div');
        lbl.style.cssText='position:absolute;top:0;left:'+x+'px;color:rgba(255,0,0,0.7);font:bold 10px monospace;padding:1px 2px;background:rgba(255,255,255,0.8)';
        lbl.textContent=x;
        g.appendChild(lbl);
      }
      for(var y=100;y<vh;y+=100){
        var lbl=document.createElement('div');
        lbl.style.cssText='position:absolute;left:0;top:'+y+'px;color:rgba(255,0,0,0.7);font:bold 10px monospace;padding:1px 2px;background:rgba(255,255,255,0.8)';
        lbl.textContent=y;
        g.appendChild(lbl);
      }
      w.appendChild(g);
    )";
  }

  // Append wrapper to document
  script += "document.documentElement.appendChild(w);";
  script += "return true;})()";

  return script;
}

// static
std::string AbpController::BuildMarkupCleanupScript(
    const std::vector<std::string>& markup_tags) {
  std::string script = "(function(){";
  script += "var o=document.getElementById('abp-markup-overlay');";
  script += "if(o)o.remove();";

  bool has_scrollable = std::find(markup_tags.begin(), markup_tags.end(),
                                  "scrollable") != markup_tags.end();
  if (has_scrollable) {
    script += "document.querySelectorAll('.abp-scrollable').forEach(function(el){";
    script += "el.classList.remove('abp-scrollable');});";
  }

  script += "})()";
  return script;
}

void AbpController::CleanupMarkupForTab(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || it->second.last_markup_tags.empty()) {
    return;
  }

  std::vector<std::string> tags = std::move(it->second.last_markup_tags);
  it->second.last_markup_tags.clear();

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) return;

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) return;

  base::Value::Dict cleanup;
  cleanup.Set("expression", BuildMarkupCleanupScript(tags));
  cleanup.Set("returnByValue", true);
  cleanup.Set("disableBreaks", true);
  VLOG(1) << "ABP: CleanupMarkupForTab fire-and-forget tab=" << tab_id;
  client->SendCommand("Runtime.evaluate", cleanup,
                      base::BindOnce([](bool, const std::string&) {}));
}

void AbpController::CaptureActionScreenshot(
    const std::string& tab_id,
    int64_t timestamp,
    bool is_before,
    const ScreenshotOptions& options,
    ActionScreenshotCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  VLOG(1) << "ABP: CaptureActionScreenshot tab=" << tab_id
            << " is_before=" << is_before
            << " markup_tags=" << options.markup_tags.size();

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    LOG(WARNING) << "ABP: CaptureActionScreenshot - WebContents not found"
                 << " tab=" << tab_id;
    std::move(callback).Run(ActionScreenshotResult());
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    LOG(WARNING) << "ABP: CaptureActionScreenshot - CDP client not found"
                 << " tab=" << tab_id;
    std::move(callback).Run(ActionScreenshotResult());
    return;
  }

  // Get viewport dimensions
  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  int view_width = 0, view_height = 0;
  if (view) {
    gfx::Size size = view->GetViewBounds().size();
    view_width = size.width();
    view_height = size.height();
  }

  // Determine history path (if history is enabled)
  base::FilePath history_path;
  if (history_controller_ && history_controller_->ScreenshotsEnabled()) {
    history_path =
        history_controller_->GetScreenshotPath(tab_id, timestamp, is_before);
  }

  // If markup tags are requested, inject overlay first
  if (!options.markup_tags.empty()) {
    std::string invalid_tag;
    if (!ValidateMarkupTags(options.markup_tags, &invalid_tag)) {
      std::move(callback).Run(ActionScreenshotResult());
      return;
    }
    std::string script = BuildMarkupInjectionScript(options.markup_tags);

    base::Value::Dict js_params;
    js_params.Set("expression", script);
    js_params.Set("returnByValue", true);
    js_params.Set("disableBreaks", true);

    VLOG(1) << "ABP PROFILE [screenshot] markup inject SEND tab=" << tab_id;
    auto markup_send = base::TimeTicks::Now();
    client->SendCommand(
        "Runtime.evaluate", js_params,
        base::BindOnce(
            [](base::WeakPtr<AbpController> ctrl, std::string tid,
               int64_t ts, bool before, ScreenshotOptions opts,
               ActionScreenshotCallback cb, base::FilePath h_path,
               int w, int h, base::TimeTicks send_time,
               bool success, const std::string& result) {
              VLOG(1) << "ABP PROFILE [screenshot] markup inject DONE"
                        << " elapsed=" << (base::TimeTicks::Now() - send_time).InMilliseconds() << "ms"
                        << " tab=" << tid;
              if (!ctrl) {
                std::move(cb).Run(ActionScreenshotResult());
                return;
              }
              ctrl->CaptureActionScreenshotCdp(
                  tid, ts, before, opts, std::move(cb), h_path, w, h);
            },
            weak_factory_.GetWeakPtr(), tab_id, timestamp, is_before, options,
            std::move(callback), history_path, view_width, view_height, markup_send));
    return;
  }

  // No markup — go directly to CDP capture
  CaptureActionScreenshotCdp(tab_id, timestamp, is_before, options,
                              std::move(callback), history_path,
                              view_width, view_height);
}

void AbpController::CaptureActionScreenshotCdp(
    const std::string& tab_id,
    int64_t timestamp,
    bool is_before,
    const ScreenshotOptions& options,
    ActionScreenshotCallback callback,
    const base::FilePath& history_path,
    int view_width,
    int view_height) {
  CaptureActionScreenshotWithRetry(tab_id, timestamp, is_before, options,
                                    std::move(callback), history_path,
                                    view_width, view_height,
                                    /*retry_count=*/0);
}

void AbpController::CaptureActionScreenshotWithRetry(
    const std::string& tab_id,
    int64_t timestamp,
    bool is_before,
    const ScreenshotOptions& options,
    ActionScreenshotCallback callback,
    const base::FilePath& history_path,
    int view_width,
    int view_height,
    int retry_count) {
  (void)is_before;

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    LOG(WARNING) << "ABP: CaptureActionScreenshotWithRetry - WebContents gone"
                 << " tab=" << tab_id;
    std::move(callback).Run(ActionScreenshotResult());
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    // RWHV can be null during cross-process navigation (renderer swap).
    // Retry up to 10 times (100ms apart, ~1s total) waiting for the new
    // renderer to attach.
    if (retry_count < 10) {
      VLOG(1) << "ABP: CaptureActionScreenshotWithRetry - no RWHV, retrying"
                << " attempt=" << retry_count << " tab=" << tab_id
                << " url=" << wc->GetLastCommittedURL().spec()
                << " loading=" << wc->IsLoading()
                << " crashed=" << wc->IsCrashed();
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&AbpController::CaptureActionScreenshotWithRetry,
                         weak_factory_.GetWeakPtr(), tab_id, timestamp,
                         is_before, options, std::move(callback),
                         history_path, view_width, view_height,
                         retry_count + 1),
          base::Milliseconds(100));
      return;
    }
    LOG(WARNING) << "ABP: CaptureActionScreenshotWithRetry - no RWHV after "
                 << retry_count << " retries, tab=" << tab_id;
    std::move(callback).Run(ActionScreenshotResult());
    return;
  }

  // ForceRedrawWithCallback uses the associated Widget Mojo pipe, which is
  // ordered with VirtualCursor (also associated). This guarantees the cursor
  // position is painted before the frame is presented.
  // If blink_widget_ is null (cross-process navigation), the callback is
  // automatically queued and retried when BindWidgetInterfaces fires.
  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      view->GetRenderWidgetHost());
  auto st = std::make_shared<ActionSnapState>();
  st->cb = std::move(callback);
  st->opts = options;
  st->h_path = history_path;
  st->tab_id = tab_id;
  st->force_redraw_start = base::TimeTicks::Now();

  // Layer 2: Watch the RWHI for destruction during in-flight ForceRedraw.
  // If the RWHI is destroyed (cross-process navigation), the watcher
  // triggers event-driven retry on the new renderer instead of waiting
  // for the 1500ms timeout.
  st->watcher = std::make_shared<ForceRedrawWatcher>(
      rwhi, weak_factory_.GetWeakPtr(), st);

  // Safety-net timeout: fall back to direct GrabViewSnapshot if ForceRedraw
  // doesn't respond in time (heavy JS pages can starve BeginMainFrame).
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<ActionSnapState> s,
             base::WeakPtr<AbpController> ctrl) {
            if (s->done) return;
            LOG(WARNING) << "ABP: CaptureActionScreenshot ForceRedraw timed out"
                         << " — falling back to direct GrabViewSnapshot";
            // Cancel the RWHI watcher — we're handling this via timeout.
            if (s->watcher) {
              s->watcher->Cancel();
              s->watcher.reset();
            }
            if (!ctrl) {
              s->done = true;
              std::move(s->cb).Run(ActionScreenshotResult());
              return;
            }
            // Try direct GrabViewSnapshot from the existing screen buffer.
            ctrl->GrabViewSnapshotWithFreshnessCheck(s, /*retry_count=*/0);
          },
          st, weak_factory_.GetWeakPtr()),
      base::Milliseconds(1500));

  VLOG(1) << "ABP PROFILE [screenshot] ForceRedraw SEND tab=" << tab_id;
  rwhi->ForceRedrawWithCallback(base::BindOnce(
      [](std::shared_ptr<ActionSnapState> s,
         base::WeakPtr<AbpController> ctrl) {
        if (s->done) return;
        // Cancel the RWHI watcher — ForceRedraw completed normally.
        if (s->watcher) {
          s->watcher->Cancel();
          s->watcher.reset();
        }
        VLOG(1) << "ABP PROFILE [screenshot] ForceRedraw DONE"
                  << " elapsed=" << (base::TimeTicks::Now() - s->force_redraw_start).InMilliseconds() << "ms"
                  << " tab=" << s->tab_id;
        if (!ctrl) {
          s->done = true;
          std::move(s->cb).Run(ActionScreenshotResult());
          return;
        }
        // Wait 50ms for CoreAnimation to composite the frame to the
        // window server buffer before OS-level capture.
        VLOG(1) << "ABP PROFILE [screenshot] CoreAnimation wait 50ms tab=" << s->tab_id;
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(
                &AbpController::GrabViewSnapshotWithFreshnessCheck,
                ctrl, s, /*retry_count=*/0),
            kCoreAnimationDelay);
      },
      st, weak_factory_.GetWeakPtr()));
}

void AbpController::GrabViewSnapshotWithFreshnessCheck(
    std::shared_ptr<ActionSnapState> s,
    int retry_count) {
  if (s->done) {
    VLOG(1) << "ABP: GrabViewSnapshotWithFreshnessCheck - already done"
              << " tab=" << s->tab_id << " retry=" << retry_count;
    return;
  }

  VLOG(1) << "ABP: GrabViewSnapshotWithFreshnessCheck - attempt"
            << " tab=" << s->tab_id << " retry=" << retry_count;

  content::WebContents* wc = FindWebContents(s->tab_id);
  if (!wc) {
    LOG(WARNING) << "ABP: GrabViewSnapshotWithFreshnessCheck - WebContents gone"
                 << " tab=" << s->tab_id;
    s->done = true;
    std::move(s->cb).Run(ActionScreenshotResult());
    return;
  }

  gfx::NativeView native_view = wc->GetContentNativeView();
  if (!native_view) {
    LOG(WARNING) << "ABP: GrabViewSnapshotWithFreshnessCheck - no native view"
                 << " tab=" << s->tab_id;
    s->done = true;
    std::move(s->cb).Run(ActionScreenshotResult());
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  gfx::Rect bounds;
  if (view) {
    bounds = gfx::Rect(view->GetViewBounds().size());
  }
  VLOG(1) << "ABP: GrabViewSnapshotWithFreshnessCheck - calling GrabViewSnapshot"
            << " bounds=" << bounds.width() << "x" << bounds.height()
            << " tab=" << s->tab_id;

  ui::GrabViewSnapshot(
      native_view, bounds,
      base::BindOnce(
          [](std::shared_ptr<ActionSnapState> s,
             base::WeakPtr<AbpController> ctrl, int retry_count,
             gfx::Image image) {
            if (s->done) return;
            VLOG(1) << "ABP: GrabViewSnapshotWithFreshnessCheck - result"
                      << " empty=" << image.IsEmpty()
                      << " retry=" << retry_count
                      << " tab=" << s->tab_id;
            if (image.IsEmpty() && retry_count < 5) {
              // Retry after 50ms — compositor may not have presented yet.
              VLOG(1) << "ABP: GrabViewSnapshotWithFreshnessCheck - retrying"
                        << " tab=" << s->tab_id;
              if (ctrl) {
                base::SingleThreadTaskRunner::GetCurrentDefault()
                    ->PostDelayedTask(
                        FROM_HERE,
                        base::BindOnce(
                            &AbpController::
                                GrabViewSnapshotWithFreshnessCheck,
                            ctrl, s, retry_count + 1),
                        kCoreAnimationDelay);
              }
              return;
            }
            s->done = true;
            if (!ctrl || image.IsEmpty()) {
              LOG(WARNING) << "ABP: GrabViewSnapshot empty after "
                           << retry_count << " retries"
                           << " tab=" << s->tab_id;
              std::move(s->cb).Run(ActionScreenshotResult());
              return;
            }
            VLOG(1) << "ABP PROFILE [screenshot] GrabViewSnapshot success"
                      << " total_from_forceredraw=" << (base::TimeTicks::Now() - s->force_redraw_start).InMilliseconds() << "ms"
                      << " retry=" << retry_count
                      << " tab=" << s->tab_id;
            ctrl->OnActionScreenshotCaptured(std::move(s->cb), s->opts,
                                             s->h_path, s->tab_id, image);
          },
          s, weak_factory_.GetWeakPtr(), retry_count));
}

void AbpController::OnActionScreenshotCaptured(
    ActionScreenshotCallback callback,
    const ScreenshotOptions& options,
    const base::FilePath& history_path,
    const std::string& tab_id,
    const gfx::Image& snapshot) {
  // Clean up markup overlay (fire-and-forget)
  if (!options.markup_tags.empty()) {
    content::WebContents* wc = FindWebContents(tab_id);
    if (wc) {
      AbpCdpClient* client = GetOrCreateCdpClient(wc);
      if (client) {
        base::Value::Dict cleanup;
        cleanup.Set("expression",
            BuildMarkupCleanupScript(options.markup_tags));
        cleanup.Set("returnByValue", true);
        cleanup.Set("disableBreaks", true);
        client->SendCommand("Runtime.evaluate", cleanup,
                            base::BindOnce([](bool, const std::string&) {}));
      }
    }
  }

  if (snapshot.IsEmpty()) {
    LOG(WARNING) << "ABP: OnActionScreenshotCaptured - snapshot empty"
                 << " tab=" << tab_id;
    std::move(callback).Run(ActionScreenshotResult());
    return;
  }

  // Read scroll position from compositor's LastRenderFrameMetadata.
  // By this point, any scroll changes from the action have been committed
  // and activated (needs_activation_notification=true ensures the viz
  // roundtrip completes, updating LastRenderFrameMetadata).
  base::Value::Dict scroll_info;
  {
    content::WebContents* scroll_wc = FindWebContents(tab_id);
    if (scroll_wc) {
      auto* scroll_view = scroll_wc->GetRenderWidgetHostView();
      if (scroll_view) {
        auto* scroll_rwhi = static_cast<content::RenderWidgetHostImpl*>(
            scroll_view->GetRenderWidgetHost());
        if (scroll_rwhi) {
          const auto& meta = scroll_rwhi->render_frame_metadata_provider()
                                 ->LastRenderFrameMetadata();
          const float dsf = meta.device_scale_factor;
          if (meta.root_scroll_offset.has_value()) {
            scroll_info.Set(
                "scrollX",
                static_cast<double>(meta.root_scroll_offset->x() / dsf));
            scroll_info.Set(
                "scrollY",
                static_cast<double>(meta.root_scroll_offset->y() / dsf));
          } else {
            scroll_info.Set("scrollX", 0.0);
            scroll_info.Set("scrollY", 0.0);
          }
          scroll_info.Set(
              "pageWidth",
              static_cast<double>(meta.root_layer_size.width() / dsf));
          scroll_info.Set(
              "pageHeight",
              static_cast<double>(meta.root_layer_size.height() / dsf));
          scroll_info.Set(
              "viewportWidth",
              static_cast<double>(
                  meta.scrollable_viewport_size.width() / dsf));
          scroll_info.Set(
              "viewportHeight",
              static_cast<double>(
                  meta.scrollable_viewport_size.height() / dsf));
        }
      }
    }
  }

  auto pipeline_start = base::TimeTicks::Now();

  const SkBitmap& raw_bitmap = *snapshot.ToSkBitmap();
  auto t1 = base::TimeTicks::Now();

  // Scale to viewport (DIP) dimensions if captured at higher device pixel ratio
  int vp_width = 0, vp_height = 0;
  content::WebContents* snap_wc = FindWebContents(tab_id);
  if (snap_wc) {
    auto* snap_view = snap_wc->GetRenderWidgetHostView();
    if (snap_view) {
      gfx::Size vp_size = snap_view->GetVisibleViewportSize();
      vp_width = vp_size.width();
      vp_height = vp_size.height();
    }
  }
  const SkBitmap bitmap = ScaleBitmapToViewport(raw_bitmap, vp_width, vp_height);
  auto t2 = base::TimeTicks::Now();

  std::optional<std::vector<uint8_t>> encoded;
  if (options.format == "webp") {
    encoded = gfx::WebpCodec::Encode(bitmap, options.quality);
  } else if (options.format == "jpeg") {
    encoded = gfx::JPEGCodec::Encode(bitmap, options.quality);
  } else {
    encoded = gfx::PNGCodec::EncodeBGRASkBitmap(
        bitmap, false /* discard_transparency */);
  }
  auto t3 = base::TimeTicks::Now();

  if (!encoded || encoded->empty()) {
    LOG(WARNING) << "ABP: OnActionScreenshotCaptured - encoding failed"
                 << " format=" << options.format << " tab=" << tab_id;
    std::move(callback).Run(ActionScreenshotResult());
    return;
  }

  ActionScreenshotResult r;
  r.base64 = base::Base64Encode(*encoded);
  auto t4 = base::TimeTicks::Now();
  r.width = bitmap.width();
  r.height = bitmap.height();
  r.scroll_info = std::move(scroll_info);

  VLOG(1) << "ABP PROFILE [screenshot] pipeline"
            << " raw=" << raw_bitmap.width() << "x" << raw_bitmap.height()
            << " scaled=" << bitmap.width() << "x" << bitmap.height()
            << " format=" << options.format << " quality=" << options.quality
            << " encoded_bytes=" << encoded->size()
            << " base64_len=" << r.base64.size()
            << " | toSkBitmap=" << (t1 - pipeline_start).InMilliseconds() << "ms"
            << " | scale=" << (t2 - t1).InMilliseconds() << "ms"
            << " | encode=" << (t3 - t2).InMilliseconds() << "ms"
            << " | base64=" << (t4 - t3).InMilliseconds() << "ms"
            << " | total=" << (t4 - pipeline_start).InMilliseconds() << "ms"
            << " tab=" << tab_id;

  // Save to disk for history if path is set
  if (!history_path.empty()) {
    auto disk_start = base::TimeTicks::Now();
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            [](base::FilePath p,
               std::vector<uint8_t> content) -> std::string {
              if (base::WriteFile(p, content)) {
                return p.AsUTF8Unsafe();
              }
              return "";
            },
            history_path, std::move(*encoded)),
        base::BindOnce(
            [](ActionScreenshotCallback final_cb,
               ActionScreenshotResult res,
               base::TimeTicks disk_start_time,
               std::string tab_id,
               std::string saved_path) {
              VLOG(1) << "ABP PROFILE [screenshot] disk_write"
                        << " elapsed=" << (base::TimeTicks::Now() - disk_start_time).InMilliseconds() << "ms"
                        << " path=" << saved_path
                        << " tab=" << tab_id;
              res.history_path = std::move(saved_path);
              std::move(final_cb).Run(std::move(res));
            },
            std::move(callback), std::move(r), disk_start, tab_id));
    return;
  }

  // No history save needed — return immediately
  std::move(callback).Run(std::move(r));
}

void AbpController::CaptureScreenshotFromBuffer(
    const std::string& tab_id,
    int64_t timestamp,
    bool is_before,
    const ScreenshotOptions& options,
    ActionScreenshotCallback callback,
    int retry_count) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  VLOG(1) << "ABP: CaptureScreenshotFromBuffer tab=" << tab_id
            << " is_before=" << is_before
            << " retry=" << retry_count;

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    LOG(WARNING) << "ABP: CaptureScreenshotFromBuffer - WebContents not found"
                 << " tab=" << tab_id;
    std::move(callback).Run(ActionScreenshotResult());
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    // RWHV can be null during cross-process navigation (renderer swap).
    // Retry up to 10 times (100ms apart, ~1s total) waiting for the new
    // renderer to attach.
    if (retry_count < 10) {
      VLOG(1) << "ABP: CaptureScreenshotFromBuffer - no RWHV, retrying"
                << " attempt=" << retry_count << " tab=" << tab_id;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&AbpController::CaptureScreenshotFromBuffer,
                         weak_factory_.GetWeakPtr(), tab_id, timestamp,
                         is_before, options, std::move(callback),
                         retry_count + 1),
          base::Milliseconds(100));
      return;
    }
    LOG(WARNING) << "ABP: CaptureScreenshotFromBuffer - no RWHV after "
                 << retry_count << " retries, tab=" << tab_id;
    std::move(callback).Run(ActionScreenshotResult());
    return;
  }

  gfx::NativeView native_view = wc->GetContentNativeView();
  if (!native_view) {
    LOG(WARNING) << "ABP: CaptureScreenshotFromBuffer - no native view"
                 << " tab=" << tab_id;
    std::move(callback).Run(ActionScreenshotResult());
    return;
  }

  gfx::Rect bounds(view->GetViewBounds().size());
  VLOG(1) << "ABP: CaptureScreenshotFromBuffer - calling GrabViewSnapshot"
            << " bounds=" << bounds.width() << "x" << bounds.height()
            << " tab=" << tab_id;

  // Determine history path (if history is enabled)
  base::FilePath history_path;
  if (history_controller_ && history_controller_->ScreenshotsEnabled()) {
    history_path =
        history_controller_->GetScreenshotPath(tab_id, timestamp, is_before);
  }

  // Grab the current screen buffer directly — no ForceRedraw needed since
  // the compositor surface is already frozen (JS paused).
  VLOG(1) << "ABP PROFILE [before_ss] GrabViewSnapshot SEND tab=" << tab_id;
  auto before_ss_send = base::TimeTicks::Now();
  ui::GrabViewSnapshot(
      native_view, bounds,
      base::BindOnce(
          [](base::WeakPtr<AbpController> ctrl,
             ActionScreenshotCallback cb,
             ScreenshotOptions opts,
             base::FilePath h_path,
             std::string tab_id,
             base::TimeTicks send_time,
             gfx::Image image) {
            VLOG(1) << "ABP PROFILE [before_ss] GrabViewSnapshot DONE"
                      << " elapsed=" << (base::TimeTicks::Now() - send_time).InMilliseconds() << "ms"
                      << " empty=" << image.IsEmpty()
                      << " tab=" << tab_id;
            if (!ctrl || image.IsEmpty()) {
              if (image.IsEmpty()) {
                LOG(WARNING) << "ABP: CaptureScreenshotFromBuffer - "
                             << "GrabViewSnapshot returned empty"
                             << " tab=" << tab_id;
              }
              std::move(cb).Run(ActionScreenshotResult());
              return;
            }
            // Reuse existing encode + save logic.  Pass empty markup_tags
            // so the cleanup branch in OnActionScreenshotCaptured is skipped.
            ScreenshotOptions clean_opts;
            clean_opts.format = opts.format;
            clean_opts.quality = opts.quality;
            ctrl->OnActionScreenshotCaptured(
                std::move(cb), clean_opts, h_path, tab_id, image);
          },
          weak_factory_.GetWeakPtr(), std::move(callback), options,
          history_path, tab_id, before_ss_send));
}

void AbpController::HandleRequest(const std::string& method,
                                  const std::string& path,
                                  const std::string& body,
                                  ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Parse JSON body if present
  base::Value::Dict params;
  if (!body.empty()) {
    auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
    if (parsed && parsed->is_dict()) {
      params = std::move(parsed->GetDict());
    }
  }

  // Strip query string before parsing path segments
  std::string query_string;
  std::string clean_path = path;
  size_t qpos = path.find('?');
  if (qpos != std::string::npos) {
    query_string = path.substr(qpos + 1);
    clean_path = path.substr(0, qpos);
  }

  // Parse path: /api/v1/tabs, /api/v1/tabs/{id}, /api/v1/tabs/{id}/action
  std::vector<std::string> segments = ParsePath(clean_path);

  // Validate /api/v1 prefix
  if (segments.size() < 3 || segments[0] != "api" || segments[1] != "v1") {
    SendError(404, "Not found", std::move(callback));
    return;
  }

  const std::string& resource = segments[2];

  // Human/CDP mode blocking guard: reject agent-loop operations when in human or CDP mode
  if (input_mode_ == InputMode::kHuman || input_mode_ == InputMode::kCdp) {
    bool blocked = false;
    std::string block_message =
        input_mode_ == InputMode::kCdp
            ? "Operation blocked: browser is in cdp mode"
            : "Operation blocked: browser is in human input mode";

    if (resource == "tabs") {
      if (input_mode_ == InputMode::kCdp) {
        if (segments.size() == 3 && method == "POST") {
          blocked = true;  // POST /tabs (create)
        }
        if (segments.size() == 4 && method == "DELETE") {
          blocked = true;  // DELETE /tabs/{id} (close)
        }
        if (segments.size() == 5 && segments[4] == "text" && method == "POST") {
          blocked = true;  // POST /tabs/{id}/text
        }
      }
      if (segments.size() == 5) {
        const std::string& action = segments[4];
        // POST /tabs/{id}/execution — specific error message
        if (action == "execution" && method == "POST") {
          SendError(
              409,
              "Execution control blocked: browser is in human input mode. "
              "Switch to agent mode first via POST "
              "/api/v1/browser/input-mode",
              std::move(callback));
          return;
        }
        // POST /tabs/{id}/screenshot — blocked (GET binary is allowed)
        if (action == "screenshot" && method == "POST") {
          blocked = true;
        }
        // Tab-scoped actions that trigger action loops
        static const base::NoDestructor<base::flat_set<std::string>>
            blocked_actions(base::flat_set<std::string>{
                "click",   "type",     "scroll",  "move",    "drag",
                "slider",  "clear_text", "execute", "wait",    "navigate",
                "reload",  "back",     "forward"});
        if (blocked_actions->contains(action) && method == "POST") {
          blocked = true;
        }
      } else if (segments.size() == 6) {
        const std::string& action = segments[4];
        const std::string& sub_action = segments[5];
        if (method == "POST") {
          // keyboard/press, keyboard/down, keyboard/up
          if (action == "keyboard" &&
              (sub_action == "press" || sub_action == "down" ||
               sub_action == "up")) {
            blocked = true;
          }
          // dialog/accept, dialog/dismiss
          if (action == "dialog" &&
              (sub_action == "accept" || sub_action == "dismiss")) {
            blocked = true;
          }
          // mouse/scroll
          if (action == "mouse" && sub_action == "scroll") {
            blocked = true;
          }
        }
      }
    } else if (resource == "file-chooser") {
      if (segments.size() == 4 && method == "POST") {
        blocked = true;
      }
    } else if (resource == "select") {
      if (segments.size() == 4 && method == "POST") {
        blocked = true;
      }
    } else if (resource == "datetime-picker") {
      if (segments.size() == 4 && method == "POST") {
        blocked = true;
      }
    } else if (resource == "permissions") {
      // POST /permissions/{id}/grant or /permissions/{id}/deny — blocked
      // GET /permissions — allowed
      if (segments.size() == 5 && method == "POST") {
        blocked = true;
      }
    }

    if (blocked) {
      SendError(409, block_message, std::move(callback));
      return;
    }
  }

  // Route: /api/v1/tabs
  if (resource == "tabs") {
    if (segments.size() == 3) {
      // /api/v1/tabs
      if (method == "GET") {
        ListTabs(std::move(callback));
      } else if (method == "POST") {
        CreateTab(params, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }

    const std::string& tab_id = segments[3];

    if (segments.size() == 4) {
      // /api/v1/tabs/{id}
      if (method == "GET") {
        GetTab(tab_id, std::move(callback));
      } else if (method == "DELETE") {
        CloseTab(tab_id, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }

    if (segments.size() == 5) {
      const std::string& action = segments[4];

      // /api/v1/tabs/{id}/{action}
      if (method != "POST" && method != "GET") {
        SendError(405, "Method not allowed", std::move(callback));
        return;
      }

      if (action == "navigate") {
        Navigate(tab_id, params, std::move(callback));
      } else if (action == "reload") {
        Reload(tab_id, std::move(callback));
      } else if (action == "back") {
        GoBack(tab_id, std::move(callback));
      } else if (action == "forward") {
        GoForward(tab_id, std::move(callback));
      } else if (action == "screenshot") {
        if (method == "GET") {
          // GET returns binary WebP directly
          BinaryScreenshot(tab_id, query_string, std::move(callback));
        } else {
          // POST returns JSON with base64 data
          Screenshot(tab_id, params, std::move(callback));
        }
      } else if (action == "execute") {
        ExecuteScript(tab_id, params, std::move(callback));
      } else if (action == "text") {
        GetText(tab_id, params, std::move(callback));
      } else if (action == "click") {
        Click(tab_id, params, std::move(callback));
      } else if (action == "type") {
        Type(tab_id, params, std::move(callback));
      } else if (action == "move") {
        Move(tab_id, params, std::move(callback));
      } else if (action == "wait") {
        Wait(tab_id, params, std::move(callback));
      } else if (action == "wait_for_network") {
        WaitForNetwork(tab_id, params, std::move(callback));
      } else if (action == "scroll") {
        Scroll(tab_id, params, std::move(callback));
      } else if (action == "drag") {
        Drag(tab_id, params, std::move(callback));
      } else if (action == "slider") {
        Slider(tab_id, params, std::move(callback));
      } else if (action == "clear_text") {
        ClearText(tab_id, params, std::move(callback));
      } else if (action == "activate") {
        ActivateTab(tab_id, std::move(callback));
      } else if (action == "stop") {
        StopLoading(tab_id, std::move(callback));
      } else if (action == "execution") {
        if (method == "GET") {
          GetExecutionState(tab_id, std::move(callback));
        } else if (method == "POST") {
          SetExecutionState(tab_id, params, std::move(callback));
        } else {
          SendError(405, "Method not allowed", std::move(callback));
        }
      } else if (action == "dialog") {
        // GET /api/v1/tabs/{id}/dialog - check for pending dialog
        if (method == "GET") {
          GetDialog(tab_id, std::move(callback));
        } else {
          SendError(405, "Method not allowed", std::move(callback));
        }
      } else if (action == "curl") {
        if (method == "POST") {
          HandleCurl(tab_id, body, std::move(callback));
        } else {
          SendError(405, "Method not allowed", std::move(callback));
        }
      } else if (action == "batch") {
        if (method != "POST") {
          SendError(405, "Method not allowed", std::move(callback));
          return;
        }
        HandleBatchRequest(tab_id, params, std::move(callback));
      } else if (action == "keyboard") {
        // Handle /api/v1/tabs/{id}/keyboard/{sub_action}
        SendError(400, "Missing keyboard sub-action (press, down, up)",
                  std::move(callback));
      } else {
        SendError(404, "Unknown action: " + action, std::move(callback));
      }
      return;
    }

    // Handle 6-segment paths: /api/v1/tabs/{id}/{action}/{sub_action}
    if (segments.size() == 6) {
      const std::string& action = segments[4];
      const std::string& sub_action = segments[5];

      if (method != "POST") {
        SendError(405, "Method not allowed", std::move(callback));
        return;
      }

      if (action == "keyboard") {
        if (sub_action == "press") {
          KeyPress(tab_id, params, std::move(callback));
        } else if (sub_action == "down") {
          KeyDown(tab_id, params, std::move(callback));
        } else if (sub_action == "up") {
          KeyUp(tab_id, params, std::move(callback));
        } else {
          SendError(404, "Unknown keyboard action: " + sub_action,
                    std::move(callback));
        }
      } else if (action == "dialog") {
        // POST /api/v1/tabs/{id}/dialog/accept or /dialog/dismiss
        if (sub_action == "accept") {
          AcceptDialog(tab_id, params, std::move(callback));
        } else if (sub_action == "dismiss") {
          DismissDialog(tab_id, std::move(callback));
        } else {
          SendError(404, "Unknown dialog action: " + sub_action,
                    std::move(callback));
        }
      } else if (action == "mouse") {
        if (sub_action == "scroll") {
          Scroll(tab_id, params, std::move(callback));
        } else {
          SendError(404, "Unknown mouse action: " + sub_action,
                    std::move(callback));
        }
      } else {
        SendError(404, "Unknown action: " + action + "/" + sub_action,
                  std::move(callback));
      }
      return;
    }
  }

  // Route: /api/v1/browser
  if (resource == "browser") {
    if (segments.size() == 4 && segments[3] == "status") {
      // /api/v1/browser/status
      if (method == "GET") {
        GetBrowserStatus(std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 4 && segments[3] == "shutdown") {
      // POST /api/v1/browser/shutdown
      if (method == "POST") {
        ShutdownBrowser(params, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 4 && segments[3] == "session-data") {
      // GET /api/v1/browser/session-data
      if (method == "GET") {
        GetSessionData(std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 4 && segments[3] == "input-mode") {
      // GET/POST /api/v1/browser/input-mode
      if (method == "GET") {
        GetInputModeResponse(std::move(callback));
      } else if (method == "POST") {
        if (input_mode_ == InputMode::kCdp) {
          SendError(409, "Operation blocked: browser is in cdp mode. Use POST /api/v1/browser/cdp-mode/exit to return to agent mode.", std::move(callback));
        } else {
          SetInputMode(params, std::move(callback));
        }
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 5 && segments[3] == "cdp-mode") {
      const std::string& cdp_action = segments[4];
      if (cdp_action == "enter" && method == "POST") {
        EnterCdpMode(params, std::move(callback));
      } else if (cdp_action == "exit" && method == "POST") {
        ExitCdpMode(std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
  }

  // Route: /api/v1/file-chooser/{id}
  if (resource == "file-chooser") {
    if (segments.size() == 4) {
      const std::string& chooser_id = segments[3];
      if (method == "POST") {
        HandleFileChooser(chooser_id, params, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
  }

  // Route: /api/v1/select/{id}
  if (resource == "select") {
    if (segments.size() == 4) {
      const std::string& popup_id = segments[3];
      if (method == "POST") {
        HandleSelectPopup(popup_id, params, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
  }

  // Route: /api/v1/datetime-picker/{id}
  if (resource == "datetime-picker") {
    if (segments.size() == 4) {
      const std::string& popup_id = segments[3];
      if (method == "POST") {
        HandleDateTimePopup(popup_id, params, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
  }

  // Route: /api/v1/permissions
  if (resource == "permissions") {
    if (segments.size() == 3) {
      // GET /api/v1/permissions
      if (method == "GET") {
        ListPendingPermissions(std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 5) {
      const std::string& perm_id = segments[3];
      const std::string& action = segments[4];
      if (method == "POST") {
        if (action == "grant") {
          GrantPermission(perm_id, params, std::move(callback));
        } else if (action == "deny") {
          DenyPermission(perm_id, params, std::move(callback));
        } else {
          SendError(404, "Unknown permission action: " + action,
                    std::move(callback));
        }
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    SendError(404, "Not found", std::move(callback));
    return;
  }

  // Route: /api/v1/downloads
  if (resource == "downloads") {
    if (segments.size() == 3) {
      // GET /api/v1/downloads
      if (method == "GET") {
        // Extract query string
        size_t query_pos = path.find('?');
        std::string query = (query_pos != std::string::npos)
                                ? path.substr(query_pos + 1)
                                : "";
        ListDownloads(query, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 4) {
      const std::string& download_id = segments[3];
      // GET /api/v1/downloads/{id}
      if (method == "GET") {
        GetDownloadStatus(download_id, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 5 && segments[4] == "cancel") {
      // POST /api/v1/downloads/{id}/cancel
      const std::string& download_id = segments[3];
      if (method == "POST") {
        CancelDownload(download_id, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 5 && segments[4] == "content") {
      // GET /api/v1/downloads/{id}/content
      const std::string& download_id = segments[3];
      if (method == "GET") {
        // Parse max_size from query string (default 10MB)
        int64_t max_size = 10 * 1024 * 1024;
        size_t query_pos = path.find('?');
        if (query_pos != std::string::npos) {
          std::string query = path.substr(query_pos + 1);
          size_t ms_pos = query.find("max_size=");
          if (ms_pos != std::string::npos) {
            std::string ms_str = query.substr(ms_pos + 9);
            size_t end = ms_str.find('&');
            if (end != std::string::npos)
              ms_str = ms_str.substr(0, end);
            int64_t parsed;
            if (base::StringToInt64(ms_str, &parsed) && parsed > 0)
              max_size = parsed;
          }
        }
        HandleDownloadContent(download_id, max_size, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
  }

  // Route: /api/v1/network[/save]
  if (resource == "network") {
    if (segments.size() == 3) {
      if (method == "GET") {
        HandleNetworkQuery(query_string, std::move(callback));
      } else if (method == "DELETE") {
        HandleNetworkClear(query_string, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 4 && segments[3] == "save") {
      if (method == "POST") {
        HandleNetworkSave(body, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    SendError(404, "Not found", std::move(callback));
    return;
  }

  // Route: /api/v1/console
  if (resource == "console") {
    if (segments.size() == 3) {
      if (method == "GET") {
        HandleConsoleQuery(query_string, std::move(callback));
      } else if (method == "DELETE") {
        HandleConsoleClear(query_string, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    SendError(404, "Not found", std::move(callback));
    return;
  }

  SendError(404, "Not found", std::move(callback));
}

void AbpController::ListTabs(ResponseCallback callback) {
  base::Value::List tabs;

  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);

      base::Value::Dict tab;
      tab.Set("id", host->GetId());
      tab.Set("url", wc->GetVisibleURL().spec());
      tab.Set("title", wc->GetTitle());
      tab.Set("active", tab_strip->active_index() == i);
      tabs.Append(std::move(tab));
    }
  }

  SendJson(200, base::Value(std::move(tabs)), std::move(callback));
}

void AbpController::GetTab(const std::string& tab_id,
                           ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  base::Value::Dict tab;
  tab.Set("id", tab_id);
  tab.Set("url", wc->GetVisibleURL().spec());
  tab.Set("title", wc->GetTitle());
  tab.Set("loading", wc->IsLoading());

  SendJson(200, base::Value(std::move(tab)), std::move(callback));
}

void AbpController::CreateTab(const base::Value::Dict& params,
                              ResponseCallback callback) {
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();

  // Get the first available browser
  Browser* browser = nullptr;
  const BrowserList* browser_list = BrowserList::GetInstance();
  auto it = browser_list->begin();
  if (it != browser_list->end()) {
    browser = *it;
  }
  if (!browser) {
    if (history_controller_) {
      history_controller_->RecordAction("", "", "create_tab", params, nullptr,
                                        false, "NO_BROWSER", "No active browser",
                                        start_time, 0, "", "");
    }
    SendError(500, "No active browser", std::move(callback));
    return;
  }

  // Ensure we're observing this browser's tab strip
  TabStripModel* tab_strip = browser->tab_strip_model();
  if (observed_tab_strips_.find(tab_strip) == observed_tab_strips_.end()) {
    tab_strip->AddObserver(this);
    observed_tab_strips_.insert(tab_strip);
  }

  // Background the currently active tab before creating a new foreground tab.
  // This releases execution control (debugger + virtual time) on the old tab
  // so the new tab doesn't show "debugger paused in another tab".
  std::string old_active_tab = GetActiveTabId();
  if (!old_active_tab.empty()) {
    BackgroundTab(old_active_tab);
  }

  const std::string* url = params.FindString("url");
  GURL gurl = url ? GURL(*url) : GURL("about:blank");

  NavigateParams nav_params(browser, gurl, ui::PAGE_TRANSITION_TYPED);
  nav_params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  ::Navigate(&nav_params);

  if (nav_params.navigated_or_inserted_contents) {
    content::WebContents* wc = nav_params.navigated_or_inserted_contents;
    auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);

    // Register popup interceptor for native popup interception
    if (popup_interceptor_) {
      wc->SetPopupInterceptor(popup_interceptor_.get());
    }
    if (permission_observer_) {
      permission_observer_->AttachToTab(host->GetId(), wc);
    }
    if (console_capture_ &&
        console_observers_.find(host->GetId()) == console_observers_.end()) {
      console_observers_[host->GetId()] = std::make_unique<AbpConsoleObserver>(
          wc, host->GetId(), console_capture_.get());
    }

    base::Value::Dict tab;
    tab.Set("id", host->GetId());
    tab.Set("url", wc->GetVisibleURL().spec());

    // Center the virtual cursor in the new tab.
    CenterCursorInTab(host->GetId(), base::DoNothing());

    // Record successful action
    if (history_controller_) {
      int64_t duration_ms =
          base::Time::Now().InMillisecondsSinceUnixEpoch() - start_time;
      base::Value result_value(tab.Clone());
      history_controller_->RecordAction("", host->GetId(), "create_tab", params,
                                        &result_value, true, "", "", start_time,
                                        duration_ms, "", "");
    }

    SendJson(201, base::Value(std::move(tab)), std::move(callback));
  } else {
    if (history_controller_) {
      history_controller_->RecordAction("", "", "create_tab", params, nullptr,
                                        false, "CREATE_FAILED",
                                        "Failed to create tab", start_time, 0,
                                        "", "");
    }
    SendError(500, "Failed to create tab", std::move(callback));
  }
}

void AbpController::CloseTab(const std::string& tab_id,
                             ResponseCallback callback) {
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  base::Value::Dict params;
  params.Set("tab_id", tab_id);

  // Use FindWebContents to locate the tab (avoids creating DevTools
  // hosts for every tab during the search).
  content::WebContents* target_wc = FindWebContents(tab_id);
  if (target_wc) {
    // Send response BEFORE closing the tab to avoid the response
    // being lost if closing triggers browser/server shutdown.
    base::Value::Dict result;
    if (history_controller_) {
      int64_t duration_ms =
          base::Time::Now().InMillisecondsSinceUnixEpoch() - start_time;
      base::Value result_value(result.Clone());
      history_controller_->RecordAction("", tab_id, "close_tab", params,
                                        &result_value, true, "", "",
                                        start_time, duration_ms, "", "");
    }
    SendJson(200, base::Value(std::move(result)), std::move(callback));

    // Detach BOTH CDP clients from the DevToolsAgentHost BEFORE closing
    // the tab.  If an AbpCdpEventClient still holds the last
    // scoped_refptr<DevToolsAgentHost> when CloseWebContentsAt destroys
    // the WebContents, AgentHostClosed drops that ref and the host is
    // destroyed mid-callback (use-after-free).
    if (event_observer_) {
      event_observer_->DetachTab(tab_id);
    }
    if (popup_interceptor_) {
      popup_interceptor_->CleanupForTab(tab_id);
    }
    if (permission_observer_) {
      permission_observer_->DetachFromTab(tab_id);
    }
    // Clean pending permissions for this tab
    for (auto it = pending_permissions_.begin();
         it != pending_permissions_.end();) {
      if (it->second.tab_id == tab_id) {
        it = pending_permissions_.erase(it);
      } else {
        ++it;
      }
    }
    CleanupTabState(tab_id);

    // Post the actual tab close to run after the current call stack
    // unwinds, so we are not inside any host/client callback chain.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](content::WebContents* wc) {
              for (Browser* b : *BrowserList::GetInstance()) {
                TabStripModel* ts = b->tab_strip_model();
                int idx = ts->GetIndexOfWebContents(wc);
                if (idx != TabStripModel::kNoTab) {
                  ts->CloseWebContentsAt(idx, TabCloseTypes::CLOSE_NONE);
                  return;
                }
              }
            },
            target_wc));
    return;
  }

  if (history_controller_) {
    history_controller_->RecordAction("", tab_id, "close_tab", params, nullptr,
                                      false, "TAB_NOT_FOUND", "Tab not found",
                                      start_time, 0, "", "");
  }
  SendError(404, "Tab not found", std::move(callback));
}

void AbpController::Navigate(const std::string& tab_id,
                             const base::Value::Dict& params,
                             ResponseCallback callback) {
  // Validate params early
  const std::string* url = params.FindString("url");
  if (!url) {
    SendError(400, "Missing 'url' parameter", std::move(callback));
    return;
  }

  GURL gurl(*url);
  if (!gurl.is_valid()) {
    SendError(400, "Invalid URL", std::move(callback));
    return;
  }

  std::string url_copy = gurl.spec();

  // Center cursor after navigation so it's in the viewport center.
  // action_before_resume: execute LoadURL before virtual time resume so that
  // the navigation IPC is queued while blink task fences are still up,
  // preventing old-page microtasks/timers from racing with teardown.
  // Debugger.disable is sent before ExecuteAction to unblock the renderer
  // main thread so beforeunload can be processed without hanging.
  auto options = GetDefaultActionOptions();
  options.center_cursor_after = true;
  options.action_before_resume = true;
  options.request_tracking_timeout = base::Milliseconds(5000);

  AbpActionContext::RunWithOptions(
      this, tab_id, "navigate", params, options,
      // Action callback - performs the actual navigation
      base::BindOnce(
          [](std::string url, AbpActionContext* ctx) {
            content::WebContents* wc = ctx->web_contents();
            if (!wc) {
              ctx->OnActionError("TAB_NOT_FOUND", "Tab not found");
              return;
            }

            GURL gurl(url);
            wc->GetController().LoadURL(gurl, content::Referrer(),
                                        ui::PAGE_TRANSITION_TYPED, std::string());

            // Set result and signal action complete
            // Wait will happen via WaitForActionComplete
            base::Value::Dict res;
            res.Set("status", "navigated");
            res.Set("url", url);
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          std::move(url_copy)),
      std::move(callback));
}

void AbpController::Reload(const std::string& tab_id,
                           ResponseCallback callback) {
  base::Value::Dict params;  // Empty params for reload

  // Center cursor after reload so it's in the viewport center.
  auto options = GetDefaultActionOptions();
  options.center_cursor_after = true;
  options.action_before_resume = true;
  options.request_tracking_timeout = base::Milliseconds(5000);

  AbpActionContext::RunWithOptions(
      this, tab_id, "reload", params, options,
      // Action callback - performs the reload
      base::BindOnce([](AbpActionContext* ctx) {
        content::WebContents* wc = ctx->web_contents();
        if (!wc) {
          ctx->OnActionError("TAB_NOT_FOUND", "Tab not found");
          return;
        }

        wc->GetController().Reload(content::ReloadType::NORMAL, false);

        base::Value::Dict res;
        res.Set("status", "reloaded");
        ctx->SetResult(std::move(res));
        ctx->OnActionDispatched();
      }),
      std::move(callback));
}

void AbpController::GoBack(const std::string& tab_id,
                           ResponseCallback callback) {
  // Early validation - check if we can go back before starting context
  content::WebContents* wc = FindWebContents(tab_id);
  if (wc && !wc->GetController().CanGoBack()) {
    SendError(400, "Cannot go back", std::move(callback));
    return;
  }

  base::Value::Dict params;  // Empty params for back

  // Center cursor after navigation so it's in the viewport center.
  auto options = GetDefaultActionOptions();
  options.center_cursor_after = true;
  options.action_before_resume = true;
  options.request_tracking_timeout = base::Milliseconds(5000);

  AbpActionContext::RunWithOptions(
      this, tab_id, "back", params, options,
      // Action callback - performs the go back
      base::BindOnce([](AbpActionContext* ctx) {
        content::WebContents* wc = ctx->web_contents();
        if (!wc) {
          ctx->OnActionError("TAB_NOT_FOUND", "Tab not found");
          return;
        }

        if (!wc->GetController().CanGoBack()) {
          ctx->OnActionError("CANNOT_GO_BACK", "Cannot go back");
          return;
        }

        wc->GetController().GoBack();

        base::Value::Dict res;
        res.Set("status", "navigated_back");
        ctx->SetResult(std::move(res));
        ctx->OnActionDispatched();
      }),
      std::move(callback));
}

void AbpController::GoForward(const std::string& tab_id,
                              ResponseCallback callback) {
  // Early validation - check if we can go forward before starting context
  content::WebContents* wc = FindWebContents(tab_id);
  if (wc && !wc->GetController().CanGoForward()) {
    SendError(400, "Cannot go forward", std::move(callback));
    return;
  }

  base::Value::Dict params;  // Empty params for forward

  // Center cursor after navigation so it's in the viewport center.
  auto options = GetDefaultActionOptions();
  options.center_cursor_after = true;
  options.action_before_resume = true;
  options.request_tracking_timeout = base::Milliseconds(5000);

  AbpActionContext::RunWithOptions(
      this, tab_id, "forward", params, options,
      // Action callback - performs the go forward
      base::BindOnce([](AbpActionContext* ctx) {
        content::WebContents* wc = ctx->web_contents();
        if (!wc) {
          ctx->OnActionError("TAB_NOT_FOUND", "Tab not found");
          return;
        }

        if (!wc->GetController().CanGoForward()) {
          ctx->OnActionError("CANNOT_GO_FORWARD", "Cannot go forward");
          return;
        }

        wc->GetController().GoForward();

        base::Value::Dict res;
        res.Set("status", "navigated_forward");
        ctx->SetResult(std::move(res));
        ctx->OnActionDispatched();
      }),
      std::move(callback));
}

void AbpController::Screenshot(const std::string& tab_id,
                               const base::Value::Dict& params,
                               ResponseCallback callback) {
  VLOG(1) << "ABP: Screenshot tab=" << tab_id;

  // Use AbpActionContext with a no-op action. This runs the full action
  // lifecycle (resume → wait → after screenshot → pause → response) which
  // shares the same proven code path as action screenshots. The screenshot
  // is captured as the "after" screenshot in the response envelope.
  auto options = GetDefaultActionOptions();
  options.min_wait_time = base::Milliseconds(100);

  AbpActionContext::RunWithOptions(
      this, tab_id, "screenshot", params, options,
      // No-op action — immediately signals dispatch complete.
      base::BindOnce([](AbpActionContext* ctx) {
        base::Value::Dict res;
        res.Set("status", "screenshot");
        ctx->SetResult(std::move(res));
        ctx->OnActionDispatched();
      }),
      std::move(callback));
}

void AbpController::ExecuteScript(const std::string& tab_id,
                                  const base::Value::Dict& params,
                                  ResponseCallback callback) {
  const std::string* script = params.FindString("script");
  if (!script) {
    SendError(400, "Missing 'script' parameter", std::move(callback));
    return;
  }

  AbpActionContext::RunWithOptions(
      this, tab_id, "execute", params, GetDefaultActionOptions(),
      base::BindOnce(
          [](std::string expression, AbpActionContext* ctx) {
            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "Failed to create CDP client");
              return;
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            base::Value::Dict cdp_params;
            cdp_params.Set("expression", expression);
            cdp_params.Set("returnByValue", true);

            client->SendCommand(
                "Runtime.evaluate", cdp_params,
                base::BindOnce(
                    [](scoped_refptr<AbpActionContext> action_ctx, bool success,
                       const std::string& result) {
                      if (!success) {
                        action_ctx->OnActionError("EXECUTE_ERROR", result);
                        return;
                      }

                      auto parsed = base::JSONReader::Read(
                          result, base::JSON_PARSE_RFC);
                      if (!parsed || !parsed->is_dict()) {
                        action_ctx->OnActionError("EXECUTE_ERROR",
                                                  "Invalid CDP response");
                        return;
                      }

                      const base::Value::Dict& dict = parsed->GetDict();

                      const base::Value::Dict* exception =
                          dict.FindDict("exceptionDetails");
                      if (exception) {
                        const base::Value::Dict* exc =
                            exception->FindDict("exception");
                        const std::string* desc =
                            exc ? exc->FindString("description") : nullptr;
                        action_ctx->OnActionError(
                            "SCRIPT_EXCEPTION",
                            desc ? *desc : "Script exception");
                        return;
                      }

                      const base::Value::Dict* cdp_result =
                          dict.FindDict("result");
                      if (cdp_result) {
                        action_ctx->SetResult(cdp_result->Clone());
                      } else {
                        action_ctx->SetResult(base::Value::Dict());
                      }
                      action_ctx->OnActionDispatched();
                    },
                    ctx_ref));
          },
          std::string(*script)),
      std::move(callback));
}


void AbpController::GetText(const std::string& tab_id,
                            const base::Value::Dict& params,
                            ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to create CDP client", std::move(callback));
    return;
  }

  // Build JS expression based on whether a selector is provided
  std::string expression;
  const std::string* selector = params.FindString("selector");
  if (selector && !selector->empty()) {
    // Escape the selector for embedding in JS string
    std::string escaped_selector = *selector;
    base::ReplaceSubstringsAfterOffset(&escaped_selector, 0, "\\", "\\\\");
    base::ReplaceSubstringsAfterOffset(&escaped_selector, 0, "'", "\\'");
    base::ReplaceSubstringsAfterOffset(&escaped_selector, 0, "\n", "\\n");
    base::ReplaceSubstringsAfterOffset(&escaped_selector, 0, "\r", "\\r");
    expression = "(function() { var el = document.querySelector('" +
                 escaped_selector +
                 "'); return el ? el.innerText : null; })()";
  } else {
    expression = "document.body ? document.body.innerText : ''";
  }

  base::Value::Dict cdp_params;
  cdp_params.Set("expression", expression);
  cdp_params.Set("returnByValue", true);
  cdp_params.Set("disableBreaks", true);

  client->SendCommand(
      "Runtime.evaluate", cdp_params,
      base::BindOnce(
          [](ResponseCallback cb, base::WeakPtr<AbpController> controller,
             bool success, const std::string& result) {
            if (!controller) return;
            if (!success) {
              controller->SendError(500, result, std::move(cb));
              return;
            }

            auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
            if (!parsed || !parsed->is_dict()) {
              controller->SendError(500, "Invalid CDP response", std::move(cb));
              return;
            }

            const base::Value::Dict& dict = parsed->GetDict();

            // Check for exception
            const base::Value::Dict* exception = dict.FindDict("exceptionDetails");
            if (exception) {
              const base::Value::Dict* exc = exception->FindDict("exception");
              const std::string* desc =
                  exc ? exc->FindString("description") : nullptr;
              controller->SendError(
                  400, desc ? *desc : "Script exception", std::move(cb));
              return;
            }

            // Get result value
            const base::Value::Dict* cdp_result = dict.FindDict("result");
            base::Value::Dict response;
            if (cdp_result) {
              const std::string* text = cdp_result->FindString("value");
              if (text) {
                response.Set("text", *text);
              } else {
                // Value might be null (selector not found)
                response.Set("text", base::Value());
              }
            } else {
              response.Set("text", "");
            }

            controller->SendJson(200, base::Value(std::move(response)),
                                 std::move(cb));
          },
          std::move(callback), weak_factory_.GetWeakPtr()));
}

void AbpController::Click(const std::string& tab_id,
                          const base::Value::Dict& params,
                          ResponseCallback callback) {
  input_dispatcher_->Click(tab_id, params, std::move(callback));
}

void AbpController::Type(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  input_dispatcher_->Type(tab_id, params, std::move(callback));
}

void AbpController::Move(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  input_dispatcher_->Move(tab_id, params, std::move(callback));
}

void AbpController::Wait(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  // Validate params early
  auto ms_opt = params.FindInt("ms");
  if (!ms_opt) {
    SendError(400, "Missing 'ms' parameter", std::move(callback));
    return;
  }

  int wait_ms = *ms_opt;
  if (wait_ms < 0) {
    SendError(400, "Wait time must be non-negative", std::move(callback));
    return;
  }
  if (wait_ms > 60000) {
    SendError(400, "Wait time must be <= 60000ms", std::move(callback));
    return;
  }

  // Use AbpActionContext with default options (resume + pause enabled)
  // This allows JavaScript to run during the wait period (for animations, etc.)
  // Flow: Resume V8 -> Wait -> Pause V8 -> Screenshot
  auto wait_options = GetDefaultActionOptions();
  wait_options.is_wait = true;  // Do NOT clear network buffer — accumulate.
  AbpActionContext::RunWithOptions(
      this, tab_id, "wait", params, wait_options,
      // Action callback - performs the wait
      base::BindOnce(
          [](int ms, AbpActionContext* ctx) {
            // Take a scoped_refptr to keep context alive through async call
            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            // Schedule the completion after the wait time
            content::GetUIThreadTaskRunner({})->PostDelayedTask(
                FROM_HERE,
                base::BindOnce(
                    [](scoped_refptr<AbpActionContext> action_ctx, int waited_ms) {
                      base::Value::Dict res;
                      res.Set("status", "waited");
                      res.Set("ms", waited_ms);
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    ctx_ref, ms),
                base::Milliseconds(ms));
          },
          wait_ms),
      std::move(callback));
}

void AbpController::WaitForNetwork(const std::string& tab_id,
                                    const base::Value::Dict& params,
                                    ResponseCallback callback) {
  AbpActionContext::Options options;
  options.min_wait_time = base::Milliseconds(250);
  options.request_tracking_timeout = base::Seconds(5);
  options.post_tracking_settle_time = base::Milliseconds(750);
  options.all_requests = true;
  if (params.FindBool("animation").value_or(false)) {
    options.animation_wait_time = base::Seconds(5);
  }

  AbpActionContext::RunWithOptions(
      this, tab_id, "wait_for_network", params, options,
      // No action — goes directly to wait phase
      ActionCallback(),
      std::move(callback));
}

void AbpController::Scroll(const std::string& tab_id,
                           const base::Value::Dict& params,
                           ResponseCallback callback) {
  input_dispatcher_->Scroll(tab_id, params, std::move(callback));
}

void AbpController::Drag(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  input_dispatcher_->Drag(tab_id, params, std::move(callback));
}

void AbpController::Slider(const std::string& tab_id,
                           const base::Value::Dict& params,
                           ResponseCallback callback) {
  input_dispatcher_->Slider(tab_id, params, std::move(callback));
}

void AbpController::ClearText(const std::string& tab_id,
                               const base::Value::Dict& params,
                               ResponseCallback callback) {
  input_dispatcher_->ClearText(tab_id, params, std::move(callback));
}

void AbpController::KeyPress(const std::string& tab_id,
                             const base::Value::Dict& params,
                             ResponseCallback callback) {
  input_dispatcher_->KeyPress(tab_id, params, std::move(callback));
}

void AbpController::KeyDown(const std::string& tab_id,
                            const base::Value::Dict& params,
                            ResponseCallback callback) {
  input_dispatcher_->KeyDown(tab_id, params, std::move(callback));
}

void AbpController::KeyUp(const std::string& tab_id,
                          const base::Value::Dict& params,
                          ResponseCallback callback) {
  input_dispatcher_->KeyUp(tab_id, params, std::move(callback));
}

// ==========================================================================
// Batch action dispatch helper (anonymous namespace)
// ==========================================================================
namespace {

// NOTE: |dispatcher| is a raw pointer to AbpController::input_dispatcher_,
// which lives for the browser session lifetime.  Batch actions complete within
// ~60ms (max 3 actions with 20ms delays), so the pointer is safe across the
// PostDelayedTask chain.  If AbpInputDispatcher gains a destructor that can
// run during tab teardown, switch to base::WeakPtr.
void DispatchBatchAction(base::Value::List actions,
                         int index,
                         std::string tab_id,
                         AbpInputDispatcher* dispatcher,
                         scoped_refptr<AbpActionContext> ctx) {
  if (index >= static_cast<int>(actions.size())) {
    // All actions dispatched
    base::Value::Dict result;
    result.Set("actions_executed", static_cast<int>(actions.size()));
    ctx->SetResult(std::move(result));
    ctx->OnActionDispatched();
    return;
  }

  const base::Value::Dict& action = actions[index].GetDict();
  const std::string* type = action.FindString("type");

  // Build params dict for the dispatcher (exclude "type" field)
  base::Value::Dict params = action.Clone();
  params.Remove("type");

  // Chain callback: schedule next action with 20ms delay (or directly if last)
  auto dispatch_next = base::BindOnce(
      [](base::Value::List actions, int next_index, std::string tab_id,
         AbpInputDispatcher* dispatcher,
         scoped_refptr<AbpActionContext> ctx) {
        if (next_index >= static_cast<int>(actions.size())) {
          // Last action — no delay needed
          DispatchBatchAction(std::move(actions), next_index,
                              std::move(tab_id), dispatcher, ctx);
          return;
        }
        content::GetUIThreadTaskRunner({})->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(&DispatchBatchAction,
                           std::move(actions), next_index,
                           std::move(tab_id), dispatcher, ctx),
            base::Milliseconds(20));
      },
      std::move(actions), index + 1, tab_id, dispatcher, ctx);

  // For keyboard_press with action param, route to press/down/up
  if (*type == "keyboard_press") {
    const std::string* key_action = params.FindString("action");
    std::string actual_action = key_action ? *key_action : "press";
    params.Remove("action");

    if (actual_action == "press") {
      dispatcher->KeyPressRaw(tab_id, params, std::move(dispatch_next));
    } else if (actual_action == "down") {
      dispatcher->KeyDownRaw(tab_id, params, std::move(dispatch_next));
    } else {
      dispatcher->KeyUpRaw(tab_id, params, std::move(dispatch_next));
    }
    return;
  }

  if (*type == "mouse_click") {
    dispatcher->ClickRaw(tab_id, params, std::move(dispatch_next));
  } else if (*type == "keyboard_type") {
    dispatcher->TypeRaw(tab_id, params, std::move(dispatch_next));
  } else if (*type == "mouse_hover") {
    dispatcher->MoveRaw(tab_id, params, std::move(dispatch_next));
  } else if (*type == "mouse_drag") {
    dispatcher->DragRaw(tab_id, params, std::move(dispatch_next));
  }
}

}  // namespace

void AbpController::HandleBatchRequest(
    const std::string& tab_id,
    const base::Value::Dict& params,
    ResponseCallback callback) {
  const base::Value::List* actions = params.FindList("actions");
  if (!actions || actions->empty()) {
    std::move(callback).Run(
        400, "application/json",
        R"({"error":"'actions' array is required and must not be empty"})");
    return;
  }
  if (actions->size() > 3) {
    std::move(callback).Run(
        400, "application/json",
        R"({"error":"'actions' array must have at most 3 elements"})");
    return;
  }

  // Get viewport size for coordinate validation
  auto* web_contents = FindWebContents(tab_id);
  if (!web_contents) {
    std::move(callback).Run(
        404, "application/json",
        R"({"error":"Tab not found"})");
    return;
  }
  gfx::Size viewport = web_contents->GetContainerBounds().size();

  // Validate all actions upfront
  base::Value::List validated_actions;
  for (size_t i = 0; i < actions->size(); i++) {
    if (!(*actions)[i].is_dict()) {
      std::move(callback).Run(
          400, "application/json",
          base::StringPrintf(
              R"({"error":"action %zu: must be an object"})", i));
      return;
    }
    const base::Value::Dict& action = (*actions)[i].GetDict();
    const std::string* type = action.FindString("type");
    if (!type) {
      std::move(callback).Run(
          400, "application/json",
          base::StringPrintf(
              R"({"error":"action %zu: missing 'type'"})", i));
      return;
    }

    base::Value::Dict validated = action.Clone();

    if (*type == "mouse_click" || *type == "mouse_hover") {
      auto x = action.FindDouble("x");
      auto y = action.FindDouble("y");
      if (!x || !y) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: %s requires 'x' and 'y'"})",
                i, type->c_str()));
        return;
      }
      if (*x < 0 || *x >= viewport.width() ||
          *y < 0 || *y >= viewport.height()) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"json({"error":"action %zu: %s coordinates (%.0f, %.0f) outside viewport (%dx%d)"})json",
                i, type->c_str(), *x, *y,
                viewport.width(), viewport.height()));
        return;
      }
    } else if (*type == "mouse_drag") {
      auto sx = action.FindDouble("start_x");
      auto sy = action.FindDouble("start_y");
      auto ex = action.FindDouble("end_x");
      auto ey = action.FindDouble("end_y");
      if (!sx || !sy || !ex || !ey) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: mouse_drag requires start_x, start_y, end_x, end_y"})",
                i));
        return;
      }
      if (*sx < 0 || *sx >= viewport.width() ||
          *sy < 0 || *sy >= viewport.height() ||
          *ex < 0 || *ex >= viewport.width() ||
          *ey < 0 || *ey >= viewport.height()) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"json({"error":"action %zu: mouse_drag coordinates outside viewport (%dx%d)"})json",
                i, viewport.width(), viewport.height()));
        return;
      }
    } else if (*type == "keyboard_type") {
      if (!action.FindString("text")) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: keyboard_type requires 'text'"})", i));
        return;
      }
    } else if (*type == "keyboard_press") {
      const std::string* key = action.FindString("key");
      if (!key) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: keyboard_press requires 'key'"})", i));
        return;
      }
      // Normalize key
      auto normalized = NormalizeKey(*key);
      if (!normalized) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: keyboard_press invalid key '%s'"})",
                i, key->c_str()));
        return;
      }
      validated.Set("key", *normalized);

      // Normalize modifiers if present
      const base::Value::List* mods = action.FindList("modifiers");
      if (mods) {
        base::Value::List normalized_mods;
        for (const auto& mod : *mods) {
          if (!mod.is_string())
            continue;
          auto norm_mod = NormalizeKey(mod.GetString());
          if (!norm_mod) {
            std::move(callback).Run(
                400, "application/json",
                base::StringPrintf(
                    R"({"error":"action %zu: keyboard_press invalid modifier '%s'"})",
                    i, mod.GetString().c_str()));
            return;
          }
          normalized_mods.Append(*norm_mod);
        }
        validated.Set("modifiers", std::move(normalized_mods));
      }

      // Validate action param if present
      const std::string* key_action = action.FindString("action");
      if (key_action && *key_action != "press" &&
          *key_action != "down" && *key_action != "up") {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: keyboard_press action must be press/down/up"})",
                i));
        return;
      }
    } else {
      std::move(callback).Run(
          400, "application/json",
          base::StringPrintf(
              R"({"error":"action %zu: unknown type '%s'. Valid: mouse_click, keyboard_type, keyboard_press, mouse_hover, mouse_drag"})",
              i, type->c_str()));
      return;
    }

    validated_actions.Append(std::move(validated));
  }

  // Extract screenshot config
  const base::Value::Dict* screenshot_config = params.FindDict("screenshot");
  base::Value::Dict sc_copy;
  if (screenshot_config) {
    sc_copy = screenshot_config->Clone();
  }

  // All validation passed — start batch execution within a single action context
  ExecuteBatchActions(tab_id, std::move(validated_actions),
                      std::move(sc_copy), std::move(callback));
}

void AbpController::ExecuteBatchActions(
    const std::string& tab_id,
    base::Value::List actions,
    base::Value::Dict screenshot_config,
    ResponseCallback callback) {

  // Build combined params for action context
  base::Value::Dict batch_params;
  batch_params.Set("actions", actions.Clone());
  if (!screenshot_config.empty()) {
    batch_params.Set("screenshot", screenshot_config.Clone());
  }

  // Use AbpActionContext for the entire batch
  AbpActionContext::RunWithOptions(
      this, tab_id, "batch", batch_params, GetDefaultActionOptions(),
      // Action callback — this runs after execution is resumed
      base::BindOnce(
          [](base::Value::List actions,
             std::string tab_id, AbpInputDispatcher* dispatcher,
             AbpActionContext* ctx) {
            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            // Dispatch all actions with 20ms delays between them
            DispatchBatchAction(std::move(actions), 0,
                                std::move(tab_id), dispatcher, ctx_ref);
          },
          std::move(actions), tab_id,
          input_dispatcher_.get()),
      std::move(callback));
}

void AbpController::ActivateTab(const std::string& tab_id,
                                ResponseCallback callback) {
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  base::Value::Dict params;
  params.Set("tab_id", tab_id);

  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      if (host->GetId() == tab_id) {
        // Background old tab, foreground new tab
        std::string old_active_tab = GetActiveTabId();
        if (!old_active_tab.empty() && old_active_tab != tab_id) {
          BackgroundTab(old_active_tab);
        }
        ForegroundTab(tab_id);

        // Activate the tab
        tab_strip->ActivateTabAt(i);

        // Also bring the browser window to front
        browser->window()->Activate();

        base::Value::Dict result;
        result.Set("status", "activated");
        result.Set("tab_id", tab_id);
        result.Set("index", i);

        if (history_controller_) {
          int64_t duration_ms =
              base::Time::Now().InMillisecondsSinceUnixEpoch() - start_time;
          base::Value result_value(result.Clone());
          history_controller_->RecordAction("", tab_id, "activate_tab", params,
                                            &result_value, true, "", "",
                                            start_time, duration_ms, "", "");
        }

        SendJson(200, base::Value(std::move(result)), std::move(callback));
        return;
      }
    }
  }

  if (history_controller_) {
    history_controller_->RecordAction("", tab_id, "activate_tab", params,
                                      nullptr, false, "TAB_NOT_FOUND",
                                      "Tab not found", start_time, 0, "", "");
  }
  SendError(404, "Tab not found", std::move(callback));
}

void AbpController::StopLoading(const std::string& tab_id,
                                ResponseCallback callback) {
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  base::Value::Dict params;
  params.Set("tab_id", tab_id);

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_) {
      history_controller_->RecordAction("", tab_id, "stop_loading", params,
                                        nullptr, false, "TAB_NOT_FOUND",
                                        "Tab not found", start_time, 0, "", "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  // Stop loading
  wc->Stop();

  base::Value::Dict result;
  result.Set("status", "stopped");
  result.Set("tab_id", tab_id);

  if (history_controller_) {
    int64_t duration_ms =
        base::Time::Now().InMillisecondsSinceUnixEpoch() - start_time;
    base::Value result_value(result.Clone());
    history_controller_->RecordAction("", tab_id, "stop_loading", params,
                                      &result_value, true, "", "",
                                      start_time, duration_ms, "", "");
  }

  SendJson(200, base::Value(std::move(result)), std::move(callback));
}

content::WebContents* AbpController::FindWebContents(
    const std::string& tab_id) {
  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      if (host->GetId() == tab_id) {
        // Lazily register popup interceptor (idempotent)
        if (popup_interceptor_ && !wc->GetPopupInterceptor()) {
          wc->SetPopupInterceptor(popup_interceptor_.get());
        }
        if (permission_observer_) {
          permission_observer_->AttachToTab(tab_id, wc);
        }
        if (console_capture_ &&
            console_observers_.find(tab_id) == console_observers_.end()) {
          console_observers_[tab_id] = std::make_unique<AbpConsoleObserver>(
              wc, tab_id, console_capture_.get());
        }
        return wc;
      }
    }
  }
  return nullptr;
}

AbpCdpClient* AbpController::GetOrCreateCdpClient(content::WebContents* wc) {
  if (input_mode_ == InputMode::kCdp) {
    return nullptr;
  }

  auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
  const std::string& id = host->GetId();

  TabState& state = GetOrCreateTabState(id);
  if (state.cdp_client) {
    return state.cdp_client.get();
  }

  auto client = std::make_unique<AbpCdpClient>(host);
  AbpCdpClient* raw_ptr = client.get();

  // Set up event listener to route CDP events to the event collector,
  // handle dialogs, and forward to action-complete wait logic if active.
  std::string tab_id = id;
  raw_ptr->SetEventListener(base::BindRepeating(
      [](base::WeakPtr<AbpController> controller, std::string tab,
         const std::string& method, const base::Value::Dict& params) {
        if (!controller) {
          return;
        }
        // Ignore events for tabs whose state has been cleaned up (e.g.
        // during tab close).  Without this check, callbacks that fire
        // during WebContents destruction would access erased map entries.
        if (controller->tab_states_.find(tab) ==
            controller->tab_states_.end()) {
          return;
        }
        // Route to event collector
        if (controller->event_collector_) {
          controller->event_collector_->OnCdpEvent(tab, method, params);
        }
        // Handle Debugger.paused event for deterministic pause confirmation
        if (method == "Debugger.paused") {
          controller->OnDebuggerPausedEvent(tab);
        }
        // Handle dialog events for pending_dialog tracking
        if (method == "Page.javascriptDialogOpening") {
          const std::string* type = params.FindString("type");
          const std::string* message = params.FindString("message");
          const std::string* default_prompt = params.FindString("defaultPrompt");
          controller->OnDialogOpened(tab, type ? *type : "alert",
                                     message ? *message : "",
                                     default_prompt ? *default_prompt : "");
        } else if (method == "Page.javascriptDialogClosed") {
          controller->OnDialogClosed(tab);
        }
        // Update persistent network tracking (always-on, before waiter check)
        controller->OnPersistentNetworkEvent(tab, method, params);
        // Forward to action-complete wait logic if a waiter is active
        controller->OnCdpEventForWait(tab, method, params);
      },
      weak_factory_.GetWeakPtr(), tab_id));

  // Enable Network domain with post body capture (50MB max)
  base::Value::Dict network_params;
  network_params.Set("maxPostDataSize", 52428800);  // 50MB
  raw_ptr->SendCommand("Network.enable", std::move(network_params),
                       base::BindOnce([](bool, const std::string&) {}));

  // Enable Page domain for navigation and dialog events
  base::Value::Dict empty_params;
  raw_ptr->SendCommand("Page.enable", empty_params,
                       base::BindOnce([](bool, const std::string&) {}));

  // Enable file chooser interception
  base::Value::Dict file_chooser_params;
  file_chooser_params.Set("enabled", true);
  raw_ptr->SendCommand("Page.setInterceptFileChooserDialog",
                       std::move(file_chooser_params),
                       base::BindOnce([](bool, const std::string&) {}));

  state.cdp_client = std::move(client);
  return raw_ptr;
}

void AbpController::SendError(int status,
                              const std::string& error,
                              ResponseCallback callback) {
  base::Value::Dict response;
  response.Set("error", error);
  SendJson(status, base::Value(std::move(response)), std::move(callback));
}

void AbpController::SendJson(int status,
                             base::Value value,
                             ResponseCallback callback) {
  std::string json;
  base::JSONWriter::Write(value, &json);
  std::move(callback).Run(status, "application/json", std::move(json));
}

void AbpController::UpdateVirtualCursorState(const std::string& tab_id,
                                             double x,
                                             double y) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    return;
  }

  scoped_refptr<content::DevToolsAgentHost> host =
      content::DevToolsAgentHost::GetOrCreateFor(wc);
  if (host) {
    TabState& tab_state = GetOrCreateTabState(host->GetId());
    tab_state.cursor.active = true;
    tab_state.cursor.x = x;
    tab_state.cursor.y = y;
  }
}

void AbpController::SetVirtualCursorViaMojo(content::WebContents* wc,
                                             float x,
                                             float y,
                                             bool visible) {
  if (!wc) {
    LOG(WARNING) << "ABP: SetVirtualCursorViaMojo - wc is null";
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    LOG(WARNING) << "ABP: SetVirtualCursorViaMojo - rwhv is null";
    return;
  }

  content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
  if (!rwh) {
    LOG(WARNING) << "ABP: SetVirtualCursorViaMojo - rwh is null";
    return;
  }

  // Check if the render process is alive before calling Mojo methods
  if (!rwh->GetProcess() || !rwh->GetProcess()->IsInitializedAndNotDead()) {
    LOG(WARNING) << "ABP: SetVirtualCursorViaMojo - render process not ready";
    return;
  }

  VLOG(1) << "ABP: SetVirtualCursorViaMojo x=" << x << " y=" << y
            << " visible=" << visible;
  rwh->SetVirtualCursorPosition(x, y, visible);
}

void AbpController::SetVirtualCursorTypeViaMojo(content::WebContents* wc,
                                                 ui::mojom::CursorType cursor_type) {
  if (!wc) {
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    return;
  }

  content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
  if (!rwh) {
    return;
  }

  rwh->SetVirtualCursorType(cursor_type);
}

void AbpController::SetVirtualCursorEnabledViaMojo(content::WebContents* wc,
                                                    bool enabled) {
  if (!wc) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorEnabledViaMojo - wc is null";
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorEnabledViaMojo - rwhv is null";
    return;
  }

  content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
  if (!rwh) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorEnabledViaMojo - rwh is null";
    return;
  }

  // Check if the render process is alive before calling Mojo methods
  if (!rwh->GetProcess() || !rwh->GetProcess()->IsInitializedAndNotDead()) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorEnabledViaMojo - render process not ready";
    return;
  }

  VLOG(1) << "ABP DEBUG L1: SetVirtualCursorEnabledViaMojo"
            << " enabled=" << enabled
            << " rwh=valid";
  rwh->SetVirtualCursorEnabled(enabled);
}

void AbpController::InsertVisualStateFence(
    const std::string& tab_id,
    base::OnceCallback<void(bool)> callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run(false);
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    std::move(callback).Run(false);
    return;
  }

  auto* rwhi =
      static_cast<content::RenderWidgetHostImpl*>(view->GetRenderWidgetHost());
  if (!rwhi || !rwhi->renderer_initialized()) {
    std::move(callback).Run(false);
    return;
  }

  // Force the compositor to produce a frame so the visual state callback fires.
  // Without this, the compositor may be idle (no animations or pending
  // repaints) and InsertVisualStateCallback would hang indefinitely waiting
  // for a frame that never comes.
  rwhi->RequestForceRedraw(/*snapshot_id=*/0);

  // Use a timeout+fallback: InsertVisualStateCallback waits for the next
  // compositor commit, but ForceRedraw goes through a different Mojo pipe
  // (blink_widget_) than VisualStateRequest (widget_compositor_), so the
  // commit may complete before the visual state request is registered.
  // If the callback doesn't fire within 500ms, proceed anyway — the cursor
  // position has already been updated via Mojo and subsequent Input.dispatch*
  // events don't require visual rendering to be committed.
  struct FenceState {
    bool done = false;
    base::OnceCallback<void(bool)> callback;
  };
  auto state = std::make_shared<FenceState>();
  state->callback = std::move(callback);

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<FenceState> s) {
            if (!s->done) {
              s->done = true;
              VLOG(1) << "ABP: InsertVisualStateFence timeout (500ms), proceeding";
              std::move(s->callback).Run(true);
            }
          },
          state),
      base::Milliseconds(500));

  rwhi->InsertVisualStateCallback(base::BindOnce(
      [](std::shared_ptr<FenceState> s, bool ready) {
        if (!s->done) {
          s->done = true;
          VLOG(1) << "ABP: InsertVisualStateFence callback fired, ready=" << ready;
          std::move(s->callback).Run(ready);
        }
      },
      state));
}

bool AbpController::IsExecutionControlEnabled() const {
  // Execution control is enabled by default, use --abp-disable-pause to disable
  return !base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kAbpDisablePause);
}

void AbpController::EnableExecutionControl(
    const std::string& tab_id,
    std::optional<double> initial_virtual_time,
    base::OnceClosure then) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    VLOG(1) << "ABP: Tab not found for execution control: " << tab_id;
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    VLOG(1) << "ABP: Failed to create CDP client for execution control";
    std::move(then).Run();
    return;
  }

  // Check if already enabled
  auto& state = GetOrCreateTabState(tab_id).execution;
  if (state.IsEnabled()) {
    std::move(then).Run();
    return;
  }

  // Step 1: Enable Debugger domain
  base::Value::Dict params;
  VLOG(1) << "ABP: Sending Debugger.enable (initial setup) for tab " << tab_id;
  client->SendCommand(
      "Debugger.enable", params,
      base::BindOnce(&AbpController::OnDebuggerEnabled,
                     weak_factory_.GetWeakPtr(), tab_id, initial_virtual_time,
                     std::move(then)));
}

void AbpController::OnDebuggerEnabled(
    const std::string& tab_id,
    std::optional<double> initial_virtual_time,
    base::OnceClosure then,
    bool success,
    const std::string& result) {
  // Guard: skip if tab was backgrounded while CDP command was inflight
  {
    auto bit = tab_states_.find(tab_id);
    if (bit != tab_states_.end() && bit->second.backgrounded) {
      VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping OnDebuggerEnabled";
      std::move(then).Run();
      return;
    }
  }

  // Log CDP ground truth
  if (success) {
    VLOG(1) << "ABP: Debugger.enable (initial setup) succeeded for tab " << tab_id;
  } else {
    LOG(WARNING) << "ABP: Debugger.enable (initial setup) failed for tab " << tab_id
                 << " - " << result;
    std::move(then).Run();
    return;
  }

  // Debugger enabled — phase will be set to kPaused in OnVirtualTimeEnabled

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(then).Run();
    return;
  }

  // Step 2: Debugger.pause FIRST (halt JS before freezing virtual time).
  // Debugger pause must happen before setVirtualTimePolicy(pause) so that
  // JS is halted before the virtual time fence is installed.
  SendDeterministicPause(
      tab_id,
      base::BindOnce(&AbpController::EnableVirtualTimeAfterDebugger,
                     weak_factory_.GetWeakPtr(), tab_id, initial_virtual_time,
                     std::move(then)));
}

void AbpController::EnableVirtualTimeAfterDebugger(
    const std::string& tab_id,
    std::optional<double> initial_virtual_time,
    base::OnceClosure then) {
  // Guard: skip if tab was backgrounded while CDP command was inflight
  {
    auto bit = tab_states_.find(tab_id);
    if (bit != tab_states_.end() && bit->second.backgrounded) {
      VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping EnableVirtualTimeAfterDebugger";
      std::move(then).Run();
      return;
    }
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(then).Run();
    return;
  }

  // Step 3: Now freeze virtual time (debugger is already paused, so
  // setVirtualTimePolicy won't trigger observer double-suspend).
  base::Value::Dict params;
  params.Set("policy", "pause");
  if (initial_virtual_time.has_value()) {
    params.Set("initialVirtualTime", *initial_virtual_time);
  }

  VLOG(1) << "ABP: Sending Emulation.setVirtualTimePolicy (pause, initial enable) for tab " << tab_id;
  client->SendCommand(
      "Emulation.setVirtualTimePolicy", params,
      base::BindOnce(&AbpController::OnVirtualTimeEnabled,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
}

void AbpController::OnVirtualTimeEnabled(
    const std::string& tab_id,
    base::OnceClosure then,
    bool success,
    const std::string& result) {
  // Guard: skip if tab was backgrounded while CDP command was inflight
  {
    auto bit = tab_states_.find(tab_id);
    if (bit != tab_states_.end() && bit->second.backgrounded) {
      VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping OnVirtualTimeEnabled";
      std::move(then).Run();
      return;
    }
  }

  // Log CDP ground truth
  if (success) {
    VLOG(1) << "ABP: Emulation.setVirtualTimePolicy (pause, initial enable) succeeded for tab " << tab_id;
  } else {
    LOG(WARNING) << "ABP: Emulation.setVirtualTimePolicy (pause, initial enable) failed for tab " << tab_id
                 << " - " << result;
    std::move(then).Run();
    return;
  }

  auto& state = GetOrCreateTabState(tab_id).execution;
  state.phase = ExecutionPhase::kPaused;

  // Parse the result to get virtualTimeTicksBase
  auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
  if (parsed && parsed->is_dict()) {
    auto ticks_base = parsed->GetDict().FindDouble("virtualTimeTicksBase");
    if (ticks_base) {
      state.virtual_time_base_ticks_ms = *ticks_base;
    }
  }

  VLOG(1) << "ABP: Execution control enabled for tab " << tab_id
            << ", virtualTimeTicksBase=" << state.virtual_time_base_ticks_ms
            << " (debugger paused, virtual time frozen)";

  // Both debugger and virtual time are paused. Done.
  std::move(then).Run();
}

void AbpController::ResumeExecution(const std::string& tab_id,
                                    base::OnceClosure then) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.execution.IsEnabled()) {
    // Execution control not enabled for this tab yet
    // If global flag is enabled, auto-enable for this tab first
    if (IsExecutionControlEnabled()) {
      EnableExecutionControl(
          tab_id, std::nullopt,
          base::BindOnce(&AbpController::ResumeExecution,
                         weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
      return;
    }
    // Global flag not enabled, just proceed
    std::move(then).Run();
    return;
  }

  ExecutionState& state = it->second.execution;
  if (!state.IsPaused()) {
    // Already resumed (or resuming)
    std::move(then).Run();
    return;
  }

  state.phase = ExecutionPhase::kResuming;

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(then).Run();
    return;
  }

  // Single atomic CDP command: Debugger.resume with disableOnResume=true.
  //
  // This resumes JS execution AND disables the debugger in a single command
  // handler, before the nested RunLoop exits.  By the time JS continues,
  // the V8 debug delegate has been removed, so page `debugger;` statements
  // (e.g. anti-debugging on Amazon/eBay) cannot re-enter a nested RunLoop.
  //
  // Without disableOnResume, resume and disable are separate IPC messages.
  // After resume exits the nested RunLoop, JS runs synchronously and can
  // hit `debugger;` before the disable message is processed — causing a
  // DCHECK crash in RenderFrameImpl::Unload when Frame::Unload arrives
  // inside the new nested RunLoop.
  //
  // SendDeterministicPause() will re-enable the Debugger domain before the
  // next ABP-initiated pause.
  base::Value::Dict resume_params;
  resume_params.Set("disableOnResume", true);
  VLOG(1) << "ABP PROFILE [resume] Debugger.resume(disableOnResume) SEND tab=" << tab_id;
  auto send_time = base::TimeTicks::Now();

  client->SendCommand(
      "Debugger.resume", resume_params,
      base::BindOnce(
          [](base::WeakPtr<AbpController> ctrl, std::string tid,
             base::OnceClosure cb, base::TimeTicks t0,
             bool success, const std::string& result) {
            VLOG(1) << "ABP PROFILE [resume] Debugger.resume(disableOnResume) DONE"
                      << " elapsed="
                      << (base::TimeTicks::Now() - t0).InMilliseconds()
                      << "ms tab=" << tid;
            if (!ctrl) return;
            if (!success) {
              LOG(WARNING) << "ABP: Debugger.resume(disableOnResume) failed tab="
                           << tid << " - " << result;
            }
            ctrl->ForceRedrawThenResumeVirtualTime(tid, std::move(cb));
          },
          weak_factory_.GetWeakPtr(), tab_id, std::move(then), send_time));
}

void AbpController::OnVirtualTimeResumed(const std::string& tab_id,
                                         base::OnceClosure then,
                                         bool success,
                                         const std::string& result) {
  // Guard: skip if tab was backgrounded while CDP command was inflight
  {
    auto bit = tab_states_.find(tab_id);
    if (bit != tab_states_.end() && bit->second.backgrounded) {
      VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping OnVirtualTimeResumed";
      std::move(then).Run();
      return;
    }
  }

  if (success) {
    LOG(INFO) << "ABP: Emulation.setVirtualTimePolicy (realtime) succeeded for tab " << tab_id
              << " result=" << result;
  } else {
    LOG(WARNING) << "ABP: Emulation.setVirtualTimePolicy (realtime) failed for tab " << tab_id
                 << " - " << result;
  }

  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end()) {
    it->second.execution.phase = ExecutionPhase::kRunning;
  }

  // Virtual time is now running (realtime policy). Proceed to the action.
  VLOG(1) << "ABP: Execution resumed for tab " << tab_id;
  std::move(then).Run();
}

void AbpController::ForceRedrawThenResumeVirtualTime(
    const std::string& tab_id,
    base::OnceClosure then) {
  // Guard: skip if tab was backgrounded while CDP command was inflight
  {
    auto bit = tab_states_.find(tab_id);
    if (bit != tab_states_.end() && bit->second.backgrounded) {
      VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping ForceRedrawThenResumeVirtualTime";
      std::move(then).Run();
      return;
    }
  }

  // Activate the tab (required — GrabViewSnapshot captures from the OS
  // compositor which only renders the active tab) but do NOT bring the
  // window to front.  ScreenCaptureKit captures by window ID regardless
  // of z-order, so window focus is unnecessary and disruptive.
  content::WebContents* wc = FindWebContents(tab_id);
  if (wc) {
    for (Browser* browser : *BrowserList::GetInstance()) {
      TabStripModel* tab_strip = browser->tab_strip_model();
      int idx = tab_strip->GetIndexOfWebContents(wc);
      if (idx != TabStripModel::kNoTab) {
        tab_strip->ActivateTabAt(idx);
        break;
      }
    }
  }

  // Skip ForceRedraw — profiling showed it always times out (3s wasted)
  // because the compositor can't produce frames while virtual time fences
  // are up. The after-screenshot path does its own ForceRedraw with virtual
  // time running, which works correctly.
  SwitchToRealtimeVirtualTime(tab_id, std::move(then));
}

void AbpController::ForceRedrawForTab(const std::string& tab_id,
                                       base::OnceClosure then) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    std::move(then).Run();
    return;
  }

  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      view->GetRenderWidgetHost());

  auto shared_cb = std::make_shared<base::OnceClosure>(std::move(then));
  auto done = std::make_shared<bool>(false);

  // Safety timeout
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<bool> d,
             std::shared_ptr<base::OnceClosure> cb) {
            if (*d) return;
            *d = true;
            LOG(WARNING) << "ABP: ForceRedrawForTab timed out after 1500ms";
            if (*cb) std::move(*cb).Run();
          },
          done, shared_cb),
      base::Milliseconds(1500));

  VLOG(1) << "ABP: ForceRedrawForTab SEND tab=" << tab_id;
  rwhi->ForceRedrawWithCallback(base::BindOnce(
      [](std::shared_ptr<bool> d,
         std::shared_ptr<base::OnceClosure> cb,
         std::string tid) {
        if (*d) return;
        *d = true;
        VLOG(1) << "ABP: ForceRedrawForTab DONE tab=" << tid;
        if (*cb) std::move(*cb).Run();
      },
      done, shared_cb, tab_id));
}

void AbpController::SwitchToRealtimeVirtualTime(
    const std::string& tab_id,
    base::OnceClosure then) {
  // Guard: skip if tab was backgrounded while CDP command was inflight
  {
    auto bit = tab_states_.find(tab_id);
    if (bit != tab_states_.end() && bit->second.backgrounded) {
      VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping SwitchToRealtimeVirtualTime";
      std::move(then).Run();
      return;
    }
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(then).Run();
    return;
  }

  base::Value::Dict params;
  params.Set("policy", "realtime");
  VLOG(1) << "ABP PROFILE [resume] setVirtualTimePolicy(realtime) SEND tab=" << tab_id;
  auto vt_send_time = base::TimeTicks::Now();
  client->SendCommand(
      "Emulation.setVirtualTimePolicy", params,
      base::BindOnce(
          [](base::WeakPtr<AbpController> ctrl, std::string tid,
             base::OnceClosure then, base::TimeTicks send_time,
             bool success, const std::string& result) {
            VLOG(1) << "ABP PROFILE [resume] setVirtualTimePolicy(realtime) DONE"
                      << " elapsed=" << (base::TimeTicks::Now() - send_time).InMilliseconds() << "ms"
                      << " tab=" << tid;
            if (ctrl) {
              ctrl->OnVirtualTimeResumed(tid, std::move(then), success, result);
            }
          },
          weak_factory_.GetWeakPtr(), tab_id, std::move(then), vt_send_time));
}

void AbpController::PauseExecution(const std::string& tab_id,
                                   base::OnceClosure then) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.execution.IsEnabled()) {
    // Execution control not enabled for this tab yet
    // If global flag is enabled, auto-enable for this tab (starts paused)
    if (IsExecutionControlEnabled()) {
      // EnableExecutionControl starts in paused state, so just enable and done
      EnableExecutionControl(tab_id, std::nullopt, std::move(then));
      return;
    }
    // Global flag not enabled, just proceed
    std::move(then).Run();
    return;
  }

  ExecutionState& state = it->second.execution;
  if (state.IsPaused()) {
    // Already paused
    std::move(then).Run();
    return;
  }

  state.phase = ExecutionPhase::kPausing;

  // Step 1: Debugger.pause FIRST (halt JS via ScopedPagePauser).
  // Debugger pause must happen before setVirtualTimePolicy(pause) so that
  // JS is halted before the virtual time fence is installed.
  SendDeterministicPause(
      tab_id,
      base::BindOnce(&AbpController::PauseVirtualTimeAfterDebugger,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
}

void AbpController::PauseVirtualTimeAfterDebugger(
    const std::string& tab_id,
    base::OnceClosure then) {
  // Guard: skip if tab was backgrounded while CDP command was inflight
  {
    auto bit = tab_states_.find(tab_id);
    if (bit != tab_states_.end() && bit->second.backgrounded) {
      VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping PauseVirtualTimeAfterDebugger";
      std::move(then).Run();
      return;
    }
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(then).Run();
    return;
  }

  // Step 2: Now freeze virtual time (debugger is already paused, so
  // setVirtualTimePolicy won't trigger observer double-suspend).
  base::Value::Dict params;
  params.Set("policy", "pause");

  VLOG(1) << "ABP PROFILE [pause] setVirtualTimePolicy(pause) SEND tab=" << tab_id;
  auto pause_vt_send = base::TimeTicks::Now();
  client->SendCommand(
      "Emulation.setVirtualTimePolicy", params,
      base::BindOnce(
          [](base::WeakPtr<AbpController> ctrl, std::string tid,
             base::OnceClosure cb, base::TimeTicks send_time,
             bool success, const std::string& result) {
            VLOG(1) << "ABP PROFILE [pause] setVirtualTimePolicy(pause) DONE"
                      << " elapsed=" << (base::TimeTicks::Now() - send_time).InMilliseconds() << "ms"
                      << " tab=" << tid;
            if (ctrl) {
              ctrl->OnVirtualTimePaused(tid, std::move(cb), success, result);
            }
          },
          weak_factory_.GetWeakPtr(), tab_id, std::move(then), pause_vt_send));
}

void AbpController::SendDeterministicPause(const std::string& tab_id,
                                           base::OnceClosure then) {
  // Guard: skip if tab was backgrounded while CDP command was inflight
  {
    auto bit = tab_states_.find(tab_id);
    if (bit != tab_states_.end() && bit->second.backgrounded) {
      VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping SendDeterministicPause";
      std::move(then).Run();
      return;
    }
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(then).Run();
    return;
  }

  // Step 1: Ensure Debugger domain is enabled before pausing.
  // After cross-process navigation (e.g. chrome:// → https://), the new
  // renderer has no CDP domains enabled. Debugger.enable is idempotent
  // so this is safe even if already enabled.
  base::Value::Dict enable_params;
  VLOG(1) << "ABP PROFILE [pause] Debugger.enable SEND tab=" << tab_id;
  auto dbg_enable_send = base::TimeTicks::Now();
  client->SendCommand(
      "Debugger.enable", enable_params,
      base::BindOnce(
          [](base::WeakPtr<AbpController> ctrl, std::string tid,
             base::OnceClosure cb, base::TimeTicks send_time,
             bool success, const std::string& result) {
            if (!ctrl) return;
            VLOG(1) << "ABP PROFILE [pause] Debugger.enable DONE"
                      << " elapsed=" << (base::TimeTicks::Now() - send_time).InMilliseconds() << "ms"
                      << " tab=" << tid;
            if (ctrl->tab_states_.count(tid) && ctrl->tab_states_[tid].backgrounded) {
              VLOG(1) << "ABP: Tab " << tid << " backgrounded, skipping pause chain after Debugger.enable";
              std::move(cb).Run();
              return;
            }
            content::WebContents* wc = ctrl->FindWebContents(tid);
            if (!wc) { std::move(cb).Run(); return; }
            AbpCdpClient* c = ctrl->GetOrCreateCdpClient(wc);
            if (!c) { std::move(cb).Run(); return; }

            // Step 2: Pause debugger (halt JS)
            base::Value::Dict params;
            VLOG(1) << "ABP PROFILE [pause] Debugger.pause SEND tab=" << tid;
            auto dbg_pause_send = base::TimeTicks::Now();
            c->SendCommand(
                "Debugger.pause", params,
                base::BindOnce(
                    [](base::WeakPtr<AbpController> ctrl2, std::string tid2,
                       base::OnceClosure cb2, base::TimeTicks send_time2,
                       bool success2, const std::string& result2) {
                      VLOG(1) << "ABP PROFILE [pause] Debugger.pause DONE"
                                << " elapsed=" << (base::TimeTicks::Now() - send_time2).InMilliseconds() << "ms"
                                << " tab=" << tid2;
                      if (!ctrl2) return;
                      if (ctrl2->tab_states_.count(tid2) && ctrl2->tab_states_[tid2].backgrounded) {
                        VLOG(1) << "ABP: Tab " << tid2 << " backgrounded, skipping pause chain after Debugger.pause";
                        std::move(cb2).Run();
                        return;
                      }
                      ctrl2->OnDebuggerPauseCommandSent(tid2, std::move(cb2), success2, result2);
                    },
                    ctrl, tid, std::move(cb), dbg_pause_send));
          },
          weak_factory_.GetWeakPtr(), tab_id, std::move(then), dbg_enable_send));
}

void AbpController::OnVirtualTimePaused(const std::string& tab_id,
                                        base::OnceClosure then,
                                        bool success,
                                        const std::string& result) {
  // Guard: skip if tab was backgrounded while CDP command was inflight
  {
    auto bit = tab_states_.find(tab_id);
    if (bit != tab_states_.end() && bit->second.backgrounded) {
      VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping OnVirtualTimePaused";
      std::move(then).Run();
      return;
    }
  }

  // Log CDP ground truth
  if (success) {
    LOG(INFO) << "ABP: Emulation.setVirtualTimePolicy (pause) succeeded for tab " << tab_id
              << " result=" << result;
  } else {
    LOG(WARNING) << "ABP: Emulation.setVirtualTimePolicy (pause) failed for tab " << tab_id
                 << " - " << result;
  }

  // Update virtual_time_base_ticks_ms from the pause response so
  // GetVirtualTimeMs() reflects the time at which virtual time was frozen.
  if (success) {
    auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
    if (parsed && parsed->is_dict()) {
      auto ticks_base = parsed->GetDict().FindDouble("virtualTimeTicksBase");
      if (ticks_base) {
        auto it = tab_states_.find(tab_id);
        if (it != tab_states_.end()) {
          it->second.execution.virtual_time_base_ticks_ms = *ticks_base;
        }
      }
    }
  }

  // Debugger is already paused and virtual time is now frozen.
  // Just run the completion callback.
  std::move(then).Run();
}

void AbpController::OnDebuggerPauseCommandSent(
    const std::string& tab_id,
    base::OnceClosure then,
    bool success,
    const std::string& result) {
  // Log CDP ground truth
  if (success) {
    VLOG(1) << "ABP: Debugger.pause command accepted for tab " << tab_id
              << " (waiting for Debugger.paused event)";
  } else {
    LOG(WARNING) << "ABP: Debugger.pause command failed for tab " << tab_id
                 << " - " << result;
  }

  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) {
    std::move(then).Run();
    return;
  }

  // Don't set phase to kPaused yet — wait for Debugger.paused EVENT.
  // Store the callback for OnDebuggerPausedEvent to fire.
  it->second.pause_event_wait_start = base::TimeTicks::Now();
  it->second.pause_completion_callback = std::move(then);

  // Start 2s safety timeout in case Debugger.paused event never arrives.
  if (!it->second.pause_confirmation_timer) {
    it->second.pause_confirmation_timer =
        std::make_unique<base::OneShotTimer>();
  }
  it->second.pause_confirmation_timer->Start(
      FROM_HERE, base::Seconds(2),
      base::BindOnce(&AbpController::OnPauseConfirmationTimeout,
                     weak_factory_.GetWeakPtr(), tab_id));

  // Send Runtime.evaluate("void 0") WITHOUT disableBreaks to force V8 to
  // execute a statement and hit the pending pause flag.
  content::WebContents* wc = FindWebContents(tab_id);
  AbpCdpClient* client = wc ? GetOrCreateCdpClient(wc) : nullptr;
  if (client) {
    base::Value::Dict params;
    params.Set("expression", "void 0");
    // NOTE: disableBreaks intentionally NOT set — we want this to trigger
    // the pending Debugger.pause.
    client->SendCommand("Runtime.evaluate", std::move(params),
                        base::BindOnce([](bool, const std::string&) {}));
  }
}

void AbpController::OnDebuggerPausedEvent(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) return;
  if (it->second.backgrounded) {
    VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, ignoring Debugger.paused event";
    return;
  }
  if (!it->second.pause_completion_callback) return;

  if (it->second.pause_confirmation_timer) {
    it->second.pause_confirmation_timer->Stop();
  }
  it->second.execution.phase = ExecutionPhase::kPaused;
  VLOG(1) << "ABP PROFILE [pause] Debugger.paused EVENT"
            << " wait=" << (base::TimeTicks::Now() - it->second.pause_event_wait_start).InMilliseconds() << "ms"
            << " tab=" << tab_id;
  if (lifecycle_observer_for_testing_) {
    lifecycle_observer_for_testing_.Run(tab_id, "", LifecycleStep::kPauseConfirmed);
  }
  std::move(it->second.pause_completion_callback).Run();
}

void AbpController::OnPauseConfirmationTimeout(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) return;
  if (it->second.backgrounded) {
    // Tab was backgrounded while waiting for Debugger.paused event.
    // Cancel the stored callback — BackgroundTab already handled cleanup.
    VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping OnPauseConfirmationTimeout";
    it->second.pause_completion_callback.Reset();
    return;
  }
  if (!it->second.pause_completion_callback) return;

  LOG(WARNING) << "ABP: Debugger.paused event not received within 2s for tab "
               << tab_id << ", proceeding anyway";
  it->second.execution.phase = ExecutionPhase::kPaused;
  if (lifecycle_observer_for_testing_) {
    lifecycle_observer_for_testing_.Run(tab_id, "", LifecycleStep::kPauseTimedOut);
  }
  std::move(it->second.pause_completion_callback).Run();
}

void AbpController::EnsureCompositorActive(const std::string& tab_id,
                                            base::OnceClosure callback) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.execution.IsPaused()) {
    // Not paused — compositor should be active already.
    std::move(callback).Run();
    return;
  }

  VLOG(1) << "ABP: EnsureCompositorActive - full resume for tab " << tab_id;

  // Guard against CDP hangs with shared_ptr + done flag + 3s timeout.
  struct GuardState {
    bool done = false;
    base::OnceClosure cb;
  };
  auto guard = std::make_shared<GuardState>();
  guard->cb = std::move(callback);

  // 3s safety timeout — ResumeExecution involves multiple CDP round-trips.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<GuardState> g) {
            if (g->done) return;
            g->done = true;
            LOG(WARNING) << "ABP: EnsureCompositorActive timed out after 3s";
            std::move(g->cb).Run();
          },
          guard),
      base::Seconds(3));

  // Full resume: Debugger.resume + setVirtualTimePolicy("realtime").
  // The renderer main thread must be unblocked for the compositor to
  // produce a frame that CopyFromSurface can grab.
  // ResumeExecution sets phase to kRunning.
  ResumeExecution(
      tab_id,
      base::BindOnce(
          [](std::shared_ptr<GuardState> g) {
            if (g->done) return;
            // Give the compositor time to produce a frame after full resume.
            base::SingleThreadTaskRunner::GetCurrentDefault()
                ->PostDelayedTask(FROM_HERE,
                    base::BindOnce(
                        [](std::shared_ptr<GuardState> g2) {
                          if (g2->done) return;
                          g2->done = true;
                          VLOG(1) << "ABP: EnsureCompositorActive completed";
                          std::move(g2->cb).Run();
                        },
                        g),
                    base::Milliseconds(100));
          },
          guard));
}

void AbpController::RestoreVirtualTimePause(const std::string& tab_id,
                                             base::OnceClosure callback) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || it->second.execution.IsPaused()) {
    // Already paused or unknown tab — nothing to do.
    std::move(callback).Run();
    return;
  }

  if (!it->second.execution.IsEnabled()) {
    // Execution control not enabled — nothing to restore.
    std::move(callback).Run();
    return;
  }

  VLOG(1) << "ABP: RestoreVirtualTimePause - full pause for tab " << tab_id;

  // Guard against CDP hangs with shared_ptr + done flag + 3s timeout.
  struct GuardState {
    bool done = false;
    base::OnceClosure cb;
  };
  auto guard = std::make_shared<GuardState>();
  guard->cb = std::move(callback);

  // 3s timeout — PauseExecution's internal 2s pause_confirmation_timer
  // fits within this outer timeout.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<GuardState> g) {
            if (g->done) return;
            g->done = true;
            LOG(WARNING) << "ABP: RestoreVirtualTimePause timed out after 3s";
            std::move(g->cb).Run();
          },
          guard),
      base::Seconds(3));

  // Full pause: setVirtualTimePolicy("pause") + Debugger.pause.
  // PauseExecution sets phase to kPaused.
  PauseExecution(
      tab_id,
      base::BindOnce(
          [](std::shared_ptr<GuardState> g) {
            if (g->done) return;
            g->done = true;
            VLOG(1) << "ABP: RestoreVirtualTimePause completed";
            std::move(g->cb).Run();
          },
          guard));
}

void AbpController::PauseAllTabs() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!IsExecutionControlEnabled()) {
    return;
  }

  VLOG(1) << "ABP: Auto-pausing all idle tabs on startup";
  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      std::string tab_id = host->GetId();

      // Register popup interceptor for native popup interception
      if (popup_interceptor_ && !wc->GetPopupInterceptor()) {
        wc->SetPopupInterceptor(popup_interceptor_.get());
      }
      if (permission_observer_) {
        permission_observer_->AttachToTab(tab_id, wc);
      }
      if (console_capture_ &&
          console_observers_.find(tab_id) == console_observers_.end()) {
        console_observers_[tab_id] = std::make_unique<AbpConsoleObserver>(
            wc, tab_id, console_capture_.get());
      }

      // Register as tab strip observer to detect page-interaction tab opens
      if (observed_tab_strips_.find(tab_strip) == observed_tab_strips_.end()) {
        tab_strip->AddObserver(this);
        observed_tab_strips_.insert(tab_strip);
      }

      // Skip tabs with an action currently executing — the action's
      // own PauseExecutionIfNeeded() will pause when it completes.
      auto it = tab_states_.find(tab_id);
      if (it != tab_states_.end() && it->second.action_in_flight) {
        VLOG(1) << "ABP: Skipping auto-pause for tab " << tab_id
                  << " (action in flight)";
        continue;
      }

      PauseExecution(tab_id, base::DoNothing());
    }
  }
}

void AbpController::BackgroundTab(const std::string& tab_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) {
    return;
  }

  TabState& tab = it->second;
  tab.backgrounded = true;

  // Cancel any pending pause confirmation timer/callback.
  if (tab.pause_confirmation_timer) {
    tab.pause_confirmation_timer->Stop();
  }
  tab.pause_completion_callback.Reset();

  VLOG(1) << "ABP: Backgrounding tab " << tab_id
          << " phase=" << static_cast<int>(tab.execution.phase);

  // If the debugger is actively paused, resume+disable atomically first.
  // Debugger.resume is NOT idempotent (errors if not paused), so only
  // call it when we know the tab is in kPaused phase.
  if (tab.execution.IsPaused()) {
    content::WebContents* wc = FindWebContents(tab_id);
    if (wc) {
      AbpCdpClient* client = GetOrCreateCdpClient(wc);
      if (client) {
        base::Value::Dict resume_params;
        resume_params.Set("disableOnResume", true);
        VLOG(1) << "ABP: BackgroundTab sending Debugger.resume(disableOnResume) tab=" << tab_id;
        client->SendCommand(
            "Debugger.resume", resume_params,
            base::BindOnce([](bool, const std::string&) {}));
      }
    }
  }

  // Unconditional Debugger.disable — idempotent safety net for any inflight
  // Debugger.enable/pause commands that may have landed in the renderer via
  // Mojo after the resume above. Mojo ordering guarantees this arrives after
  // any inflight commands on the same channel.
  content::WebContents* wc = FindWebContents(tab_id);
  if (wc) {
    AbpCdpClient* client = GetOrCreateCdpClient(wc);
    if (client) {
      base::Value::Dict empty;
      VLOG(1) << "ABP: BackgroundTab sending Debugger.disable tab=" << tab_id;
      client->SendCommand(
          "Debugger.disable", empty,
          base::BindOnce([](bool, const std::string&) {}));

      // Release virtual time fences if execution control was ever enabled.
      if (tab.execution.IsEnabled()) {
        base::Value::Dict vt_params;
        vt_params.Set("policy", "realtime");
        VLOG(1) << "ABP: BackgroundTab sending setVirtualTimePolicy(realtime) tab=" << tab_id;
        client->SendCommand(
            "Emulation.setVirtualTimePolicy", vt_params,
            base::BindOnce([](bool, const std::string&) {}));
      }
    }
  }

  tab.execution.phase = ExecutionPhase::kDisabled;
}

void AbpController::ForegroundTab(const std::string& tab_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end()) {
    it->second.backgrounded = false;
  } else {
    GetOrCreateTabState(tab_id).backgrounded = false;
  }

  VLOG(1) << "ABP: Foregrounding tab " << tab_id;

  // Don't immediately pause the tab here. When a click opens a new tab
  // (window.open / target=_blank), pausing it immediately causes
  // "debugger paused in another tab" because both tabs share the same
  // renderer process and ScopedPagePauser affects all pages in the
  // renderer. Instead, let the tab load freely — execution control will
  // be established when the next action targets this tab (PauseExecution
  // at end of action lifecycle).
}

void AbpController::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // We only care about active tab changes (selection changes).
  // This catches page-interaction tab opens (window.open, target=_blank)
  // where Chrome automatically foregrounds the new tab.
  if (!selection.active_tab_changed()) {
    return;
  }

  // Don't react if execution control is not enabled globally.
  if (!IsExecutionControlEnabled()) {
    return;
  }

  // Resolve new tab ID early — needed for both background and foreground logic.
  std::string new_tab_id;
  if (selection.new_contents) {
    auto new_host = content::DevToolsAgentHost::GetOrCreateFor(
        selection.new_contents);
    new_tab_id = new_host->GetId();
  }

  // Background the old active tab
  if (selection.old_contents) {
    auto old_host = content::DevToolsAgentHost::GetOrCreateFor(
        selection.old_contents);
    std::string old_tab_id = old_host->GetId();

    auto it = tab_states_.find(old_tab_id);
    if (it != tab_states_.end() && !it->second.backgrounded) {
      // If an action is in flight on the old tab, record the tab switch
      // so the action context can redirect screenshot/pause to the new tab.
      if (it->second.action_in_flight && !new_tab_id.empty()) {
        it->second.tab_switched_to = new_tab_id;
        VLOG(1) << "ABP: Tab switch during action — old=" << old_tab_id
                << " new=" << new_tab_id;
      }
      BackgroundTab(old_tab_id);
    }
  }

  // Foreground the new active tab
  if (!new_tab_id.empty()) {
    auto it = tab_states_.find(new_tab_id);
    // Only foreground if not already foregrounded (avoid duplicate work
    // when ActivateTab already called ForegroundTab).
    if (it != tab_states_.end() && it->second.backgrounded) {
      ForegroundTab(new_tab_id);
    }
  }
}

// ==========================================================================
// Input mode management
// ==========================================================================

void AbpController::AddInputModeObserver(InputModeObserver* observer) {
  input_mode_observers_.AddObserver(observer);
}

void AbpController::RemoveInputModeObserver(InputModeObserver* observer) {
  input_mode_observers_.RemoveObserver(observer);
}

void AbpController::GetInputModeResponse(ResponseCallback callback) {
  base::Value::Dict response;
  const char* mode_str = "agent";
  if (input_mode_ == InputMode::kHuman) mode_str = "human";
  else if (input_mode_ == InputMode::kCdp) mode_str = "cdp";
  response.Set("input_mode", mode_str);
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::SetInputMode(const base::Value::Dict& params,
                                 ResponseCallback callback) {
  if (input_mode_ == InputMode::kCdp) {
    SendError(409, "Cannot change input mode while in CDP mode. Exit CDP mode first.", std::move(callback));
    return;
  }
  const std::string* mode_str = params.FindString("input_mode");
  if (!mode_str) {
    SendError(400, "Missing 'input_mode' parameter", std::move(callback));
    return;
  }

  if (*mode_str == "human") {
    if (input_mode_ == InputMode::kHuman) {
      GetInputModeResponse(std::move(callback));
      return;
    }
    SwitchToHumanMode(std::move(callback));
  } else if (*mode_str == "agent") {
    if (input_mode_ == InputMode::kAgent) {
      GetInputModeResponse(std::move(callback));
      return;
    }
    SwitchToAgentMode(std::move(callback));
  } else {
    SendError(400, "Invalid input_mode: must be 'agent' or 'human'",
              std::move(callback));
  }
}

void AbpController::SwitchToHumanMode(ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // 1. Abort in-flight actions for all tabs.
  for (auto& [tab_id, state] : tab_states_) {
    AbortActiveAction(tab_id);
  }

  // 2. Save execution state and resume paused tabs.
  saved_execution_states_.clear();
  for (auto& [tab_id, state] : tab_states_) {
    if (state.execution.IsEnabled()) {
      SavedExecutionState saved;
      saved.was_enabled = true;
      saved.was_paused = state.execution.IsPaused();
      saved.virtual_time_base_ms = state.execution.virtual_time_base_ticks_ms;
      saved_execution_states_[tab_id] = saved;

      if (state.execution.IsPaused()) {
        ResumeExecution(tab_id, base::DoNothing());
      }
    }
  }

  // 3. Allow system inputs on all tabs.
  SetAllowSystemInputsForAllTabs(true);

  // 4. Update state and notify observers.
  input_mode_ = InputMode::kHuman;
  for (auto& observer : input_mode_observers_) {
    observer.OnInputModeChanged(InputMode::kHuman);
  }

  GetInputModeResponse(std::move(callback));
}

void AbpController::SwitchToAgentMode(ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // 1. Block system inputs on all tabs.
  SetAllowSystemInputsForAllTabs(false);

  // 2. Restore saved execution state — re-pause tabs that were paused.
  for (auto& [tab_id, saved] : saved_execution_states_) {
    if (saved.was_enabled && saved.was_paused) {
      auto it = tab_states_.find(tab_id);
      if (it != tab_states_.end() && it->second.execution.IsEnabled()) {
        PauseExecution(tab_id, base::DoNothing());
      }
    }
  }
  saved_execution_states_.clear();

  // 3. Update state and notify observers.
  input_mode_ = InputMode::kAgent;
  for (auto& observer : input_mode_observers_) {
    observer.OnInputModeChanged(InputMode::kAgent);
  }

  GetInputModeResponse(std::move(callback));
}

void AbpController::EnterCdpMode(const base::Value::Dict& params,
                                  ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (input_mode_ == InputMode::kCdp) {
    SendError(409, "already in cdp mode", std::move(callback));
    return;
  }

  int requested_port = params.FindInt("port").value_or(0);
  int port;
  if (requested_port > 0) {
    port = FindAvailableCdpPort(requested_port, 1);
  } else {
    port = FindAvailableCdpPort(24578, 100);
  }
  if (port < 0) {
    SendError(503, "no available port", std::move(callback));
    return;
  }

  // Suspend ABP: abort actions, save execution state, resume paused tabs.
  if (input_mode_ == InputMode::kAgent) {
    for (auto& [tab_id, state] : tab_states_) {
      AbortActiveAction(tab_id);
    }
    saved_execution_states_.clear();
    for (auto& [tab_id, state] : tab_states_) {
      if (state.execution.IsEnabled()) {
        SavedExecutionState saved;
        saved.was_enabled = true;
        saved.was_paused = state.execution.IsPaused();
        saved.virtual_time_base_ms = state.execution.virtual_time_base_ticks_ms;
        saved_execution_states_[tab_id] = saved;
        if (state.execution.IsPaused()) {
          ResumeExecution(tab_id, base::DoNothing());
        }
      }
    }
    SetAllowSystemInputsForAllTabs(true);
  }

  // Detach all ABP CDP clients so the remote debugging server can attach.
  DetachAllCdpClients();

  // Start Chrome's remote debugging server on the selected port.
  // The socket factory writes the actual bound address to cdp_bound_endpoint_
  // from the handler thread so we can construct an accurate ws_url.
  cdp_bound_endpoint_ = std::make_shared<CdpBoundEndpoint>();
  content::DevToolsAgentHost::StartRemoteDebuggingServer(
      std::make_unique<AbpCdpSocketFactory>(port, cdp_bound_endpoint_),
      base::FilePath(), base::FilePath(),
      content::DevToolsAgentHost::RemoteDebuggingServerMode::kDefault);

  cdp_port_ = port;
  // Initial ws_url uses the address from the pre-test bind probe.
  // GetBrowserStatus will read the actual bound address from cdp_bound_endpoint_
  // once the server has started on the handler thread.
  cdp_ws_url_ = "ws://" + cdp_bound_address_ + ":" + std::to_string(port) + "/devtools/browser";

  // Optional auto-exit timeout.
  std::optional<int> timeout_ms = params.FindInt("timeout_ms");
  if (timeout_ms.has_value() && *timeout_ms > 0) {
    cdp_timeout_deadline_ =
        base::TimeTicks::Now() + base::Milliseconds(*timeout_ms);
    cdp_timeout_timer_.Start(
        FROM_HERE, base::Milliseconds(*timeout_ms),
        base::BindOnce(&AbpController::OnCdpModeTimeout,
                        base::Unretained(this)));
  } else {
    cdp_timeout_deadline_ = base::TimeTicks();
  }

  input_mode_ = InputMode::kCdp;
  for (auto& observer : input_mode_observers_) {
    observer.OnInputModeChanged(InputMode::kCdp);
  }

  base::Value::Dict response;
  response.Set("status", "ok");
  response.Set("port", cdp_port_);
  response.Set("ws_url", cdp_ws_url_);
  if (timeout_ms.has_value() && *timeout_ms > 0) {
    response.Set("timeout_ms", *timeout_ms);
  }
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::OnCdpModeTimeout() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (input_mode_ != InputMode::kCdp) {
    return;
  }
  VLOG(1) << "ABP: CDP mode timeout — auto-exiting";
  ExitCdpMode(
      base::BindOnce([](int, const std::string&, std::string) {}));
}

void AbpController::ExitCdpMode(ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (input_mode_ != InputMode::kCdp) {
    SendError(409, "not in cdp mode", std::move(callback));
    return;
  }

  // Stop Chrome's remote debugging server.
  content::DevToolsAgentHost::StopRemoteDebuggingServer();
  cdp_port_ = 0;
  cdp_ws_url_.clear();
  cdp_bound_address_.clear();
  cdp_bound_endpoint_.reset();

  // Cancel auto-exit timeout.
  cdp_timeout_timer_.Stop();
  cdp_timeout_deadline_ = base::TimeTicks();

  // Set to kAgent before re-attach so GetOrCreateCdpClient guard allows it.
  input_mode_ = InputMode::kAgent;

  // Re-attach ABP CDP clients to all live tabs.
  ReattachAllCdpClients();

  // Block system inputs again (ABP-controlled mode).
  SetAllowSystemInputsForAllTabs(false);

  // Restore saved execution state — re-pause tabs that were paused.
  for (auto& [tab_id, saved] : saved_execution_states_) {
    if (saved.was_enabled && saved.was_paused) {
      auto it = tab_states_.find(tab_id);
      if (it != tab_states_.end() && it->second.execution.IsEnabled()) {
        PauseExecution(tab_id, base::DoNothing());
      }
    }
  }
  saved_execution_states_.clear();

  // Notify observers of mode change.
  for (auto& observer : input_mode_observers_) {
    observer.OnInputModeChanged(InputMode::kAgent);
  }

  base::Value::Dict response;
  response.Set("status", "ok");
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::AbortActiveAction(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) {
    return;
  }

  TabState& state = it->second;
  if (!state.action_in_flight) {
    return;
  }

  // Invalidate the current action epoch. The active AbpActionContext will
  // see IsCurrentAction() return false at its next async checkpoint and
  // self-destruct, releasing the HTTP response callback (closing the
  // connection).
  state.action_in_flight = false;
  state.active_action_epoch = 0;

  // Drain queued actions — they'll discover the tab has no active slot
  // and self-destruct similarly.
  state.queued_action_starters.clear();
}

void AbpController::SetAllowSystemInputsForAllTabs(bool allow) {
  const BrowserList* browser_list = BrowserList::GetInstance();
  for (auto it = browser_list->begin(); it != browser_list->end(); ++it) {
    Browser* browser = *it;
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      if (!wc) {
        continue;
      }
      content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
      if (!view) {
        continue;
      }
      auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
          view->GetRenderWidgetHost());
      if (!rwhi) {
        continue;
      }
      input::RenderInputRouter* router = rwhi->GetRenderInputRouter();
      if (router) {
        router->SetAllowSystemInputs(allow);
      }
    }
  }
}

int AbpController::FindAvailableCdpPort(int start_port, int max_attempts) {
  for (int i = 0; i < max_attempts; ++i) {
    int port = start_port + i;
    // Try IPv4 first (matches AbpCdpSocketFactory order).
    auto socket = std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    net::IPEndPoint endpoint(net::IPAddress::IPv4Localhost(), port);
    if (socket->Listen(endpoint, 1, std::nullopt) == net::OK) {
      cdp_bound_address_ = "127.0.0.1";
      return port;
    }
    // Try IPv6 fallback.
    socket = std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    net::IPEndPoint endpoint6(net::IPAddress::IPv6Localhost(), port);
    if (socket->Listen(endpoint6, 1, std::nullopt) == net::OK) {
      cdp_bound_address_ = "[::1]";
      return port;
    }
  }
  return -1;
}

void AbpController::DetachAllCdpClients() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  for (auto& [tab_id, state] : tab_states_) {
    state.cdp_client.reset();
  }
  if (event_observer_) {
    for (auto& [tab_id, state] : tab_states_) {
      event_observer_->DetachTab(tab_id);
    }
  }
}

void AbpController::ReattachAllCdpClients() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (event_observer_) {
    event_observer_->Stop();
    event_observer_->Start();
  }
  std::set<std::string> live_tab_ids;
  const BrowserList* browser_list = BrowserList::GetInstance();
  for (auto it = browser_list->begin(); it != browser_list->end(); ++it) {
    Browser* browser = *it;
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      if (!wc) continue;
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      const std::string& tab_id = host->GetId();
      live_tab_ids.insert(tab_id);
      GetOrCreateCdpClient(wc);
      if (!tab_states_.count(tab_id) || !tab_states_[tab_id].execution.IsEnabled()) {
        if (IsExecutionControlEnabled()) {
          EnableExecutionControl(tab_id, std::nullopt, base::DoNothing());
        }
      }
    }
  }
  std::vector<std::string> stale_ids;
  for (auto& [tab_id, state] : tab_states_) {
    if (!live_tab_ids.contains(tab_id)) {
      stale_ids.push_back(tab_id);
    }
  }
  for (const auto& tab_id : stale_ids) {
    tab_states_.erase(tab_id);
  }
}

void AbpController::GetExecutionState(const std::string& tab_id,
                                      ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  base::Value::Dict response;

  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end()) {
    const ExecutionState& state = it->second.execution;
    response.Set("enabled", state.IsEnabled());
    response.Set("paused", state.IsPaused());
    response.Set("virtual_time_base_ms", state.virtual_time_base_ticks_ms);
  } else {
    response.Set("enabled", false);
    response.Set("paused", false);
    response.Set("virtual_time_base_ms", 0.0);
  }

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::SetExecutionState(const std::string& tab_id,
                                      const base::Value::Dict& params,
                                      ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  auto tab_state_it = tab_states_.find(tab_id);
  if (tab_state_it != tab_states_.end() && tab_state_it->second.action_in_flight) {
    SendError(409, "Cannot change execution state while an action is in flight",
              std::move(callback));
    return;
  }

  auto paused = params.FindBool("paused");
  if (!paused.has_value()) {
    SendError(400, "Missing 'paused' parameter", std::move(callback));
    return;
  }

  // Check if execution control is enabled for this tab
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.execution.IsEnabled()) {
    // Need to enable first
    std::optional<double> initial_time;
    auto init_time = params.FindDouble("initial_virtual_time");
    if (init_time) {
      initial_time = *init_time;
    }

    bool target_paused = *paused;
    EnableExecutionControl(
        tab_id, initial_time,
        base::BindOnce(
            [](base::WeakPtr<AbpController> controller, std::string tid,
               bool target_paused, ResponseCallback cb) {
              if (!controller) {
                return;
              }
              if (target_paused) {
                // Already starts paused, just respond
                controller->GetExecutionState(tid, std::move(cb));
              } else {
                // Need to resume
                controller->ResumeExecution(
                    tid,
                    base::BindOnce(
                        [](base::WeakPtr<AbpController> ctrl, std::string id,
                           ResponseCallback callback) {
                          if (ctrl) {
                            ctrl->GetExecutionState(id, std::move(callback));
                          }
                        },
                        controller, tid, std::move(cb)));
              }
            },
            weak_factory_.GetWeakPtr(), tab_id, target_paused,
            std::move(callback)));
    return;
  }

  // Already enabled, just pause or resume
  if (*paused) {
    PauseExecution(
        tab_id,
        base::BindOnce(
            [](base::WeakPtr<AbpController> controller, std::string tid,
               ResponseCallback cb) {
              if (controller) {
                controller->GetExecutionState(tid, std::move(cb));
              }
            },
            weak_factory_.GetWeakPtr(), tab_id, std::move(callback)));
  } else {
    ResumeExecution(
        tab_id,
        base::BindOnce(
            [](base::WeakPtr<AbpController> controller, std::string tid,
               ResponseCallback cb) {
              if (controller) {
                controller->GetExecutionState(tid, std::move(cb));
              }
            },
            weak_factory_.GetWeakPtr(), tab_id, std::move(callback)));
  }
}

// =============================================================================
// Action complete wait implementation
// =============================================================================

// Constants for action_complete wait
// Note: min_wait_time is now configurable per-action via WaitForActionComplete param
namespace {
constexpr base::TimeDelta kNetworkIdleTime = base::Milliseconds(500);
constexpr base::TimeDelta kNetworkIdleCheckInterval = base::Milliseconds(100);
constexpr base::TimeDelta kWaitTimeout = base::Seconds(10);
constexpr int kNetworkIdleMaxConnections = 2;  // networkidle2
}  // namespace

void AbpController::WaitForActionComplete(
    const std::string& tab_id,
    base::OnceClosure on_complete,
    base::TimeDelta min_wait_time,
    base::TimeDelta request_tracking_timeout,
    base::TimeDelta post_tracking_settle_time,
    bool page_was_loaded_before_action,
    bool all_requests,
    base::TimeDelta animation_wait_time) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    // Tab not found, call callback immediately
    std::move(on_complete).Run();
    return;
  }

  // Create waiter state
  auto waiter = std::make_unique<ActionCompleteWaiter>();
  waiter->tab_id = tab_id;
  waiter->action_start_time = base::TimeTicks::Now();
  waiter->on_complete = std::move(on_complete);
  waiter->last_network_activity = base::TimeTicks::Now();
  waiter->timeout_time = base::TimeTicks::Now() + kWaitTimeout;
  waiter->min_wait_time = min_wait_time;
  waiter->request_tracking_timeout = request_tracking_timeout;
  waiter->post_tracking_settle_time = post_tracking_settle_time;
  waiter->all_requests = all_requests;

  // Start animation timer immediately (not gated on load events).
  // Runs in parallel with all other wait conditions.
  if (!animation_wait_time.is_zero()) {
    waiter->animation_timer_started = true;
    VLOG(1) << "ABP PROFILE [wait] animation_timer STARTED"
            << " duration=" << animation_wait_time.InMilliseconds() << "ms"
            << " tab=" << tab_id;
    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpController::OnAnimationWaitTimeElapsed,
                       weak_factory_.GetWeakPtr(), tab_id),
        animation_wait_time);
  }

  // Extract page's registrable domain (eTLD+1) for same-site request filtering.
  // Only requests to this domain or its subdomains will be tracked.
  // Use GetVisibleURL() instead of GetLastCommittedURL() because during
  // navigation the committed URL is still the OLD page — GetVisibleURL()
  // reflects the target URL as soon as navigation begins.
  {
    GURL page_url = wc->GetVisibleURL();
    waiter->page_registrable_domain =
        net::registry_controlled_domains::GetDomainAndRegistry(
            page_url,
            net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  }

  bool currently_loading = wc->IsLoading();
  // Use the pre-resume snapshot: if the page was loaded before the action
  // started (before JS was resumed), skip waiting for load events even if
  // the page is now loading due to action-triggered resource loads (ads etc).
  bool skip_load_events = page_was_loaded_before_action || !currently_loading;
  VLOG(1) << "ABP PROFILE [wait] START tab=" << tab_id
            << " page_loaded_before=" << page_was_loaded_before_action
            << " currently_loading=" << currently_loading
            << " skip_load_events=" << skip_load_events
            << " min_wait=" << min_wait_time.InMilliseconds() << "ms"
            << " tracking_timeout=" << request_tracking_timeout.InMilliseconds() << "ms"
            << " settle=" << post_tracking_settle_time.InMilliseconds() << "ms"
            << " animation=" << animation_wait_time.InMilliseconds() << "ms";

  // For pages that were already loaded (either before the action or right now),
  // set load events as fired so the min_wait timer starts immediately.
  // Action-triggered resource loads (ads, analytics, XHR) should not delay
  // the min_wait timer — they'll be caught by the request tracking phase.
  if (skip_load_events) {
    waiter->load_fired = true;
    waiter->dom_content_loaded_fired = true;
    waiter->first_paint_fired = true;
  }

  // Create page load observer for load lifecycle events.  Uses browser-side
  // WebContentsObserver callbacks instead of CDP Page domain events, avoiding
  // race conditions with DevToolsSession message suspension during navigation.
  waiter->page_load_observer = std::make_unique<AbpPageLoadObserver>(
      wc, base::BindRepeating(&AbpController::OnPageLifecycleEvent,
                               weak_factory_.GetWeakPtr(), tab_id));

  auto& tab_state = GetOrCreateTabState(tab_id);
  waiter->waiter_epoch = ++tab_state.next_waiter_epoch;
  tab_state.action_waiter = std::move(waiter);

  // Enable Network domain for request tracking (all actions)
  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (client) {
    base::Value::Dict empty_params;
    client->SendCommand("Network.enable", empty_params,
                        base::BindOnce([](bool, const std::string&) {}));
  }

  // Min wait timer starts in MaybeStartMinWaitTimer once all base conditions
  // (load + dom_content_loaded + first_paint) are met. Check now in case
  // the page is already loaded.
  MaybeStartMinWaitTimer(tab_id);

  // Start timeout timer (absolute safety net).
  // Pass waiter_epoch so stale timeouts from prior actions are ignored.
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnWaitTimeout,
                     weak_factory_.GetWeakPtr(), tab_id,
                     tab_state.action_waiter->waiter_epoch),
      kWaitTimeout);
}

void AbpController::OnCdpEventForWait(const std::string& tab_id,
                                      const std::string& method,
                                      const base::Value::Dict& params) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();

  // Main-frame navigation during wait: reset load flags and domain so we
  // wait for the NEW page's first paint / DCL / load events and track
  // same-site requests against the new domain.
  if (method == "Page.frameNavigated") {
    const base::Value::Dict* frame = params.FindDict("frame");
    if (frame) {
      const std::string* parent_id = frame->FindString("parentId");
      if (!parent_id) {  // main frame only
        const std::string* url = frame->FindString("url");
        VLOG(1) << "ABP PROFILE [wait] main-frame navigation detected"
                << " url=" << (url ? *url : "?")
                << " elapsed="
                << (base::TimeTicks::Now() - waiter->action_start_time)
                       .InMilliseconds()
                << "ms tab=" << tab_id;

        // Reset page load flags — must re-fire for the new page.
        // Don't gate on `load` (waits for ALL subresources including ads);
        // DCL + first_paint is sufficient to know the page is interactive.
        waiter->load_fired = true;
        waiter->dom_content_loaded_fired = false;
        waiter->first_paint_fired = false;

        // Update registrable domain from the new URL
        if (url) {
          GURL new_url(*url);
          waiter->page_registrable_domain =
              net::registry_controlled_domains::GetDomainAndRegistry(
                  new_url,
                  net::registry_controlled_domains::
                      INCLUDE_PRIVATE_REGISTRIES);
        }

        // Clear stale request tracking from the old page
        waiter->active_request_ids.clear();
        waiter->tracked_requests.clear();
        waiter->active_requests = 0;
        waiter->tracking_snapshot_taken = false;
        waiter->tracked_requests_resolved = false;
        waiter->tracking_timed_out = false;
        waiter->post_tracking_settle_started = false;
        waiter->post_tracking_settled = false;

        // Reset min-wait timer so Phase 1 restarts for the new page
        waiter->min_time_elapsed = false;
        waiter->min_wait_timer_started = false;
        MaybeStartMinWaitTimer(tab_id);
      }
    }
    return;
  }

  // Track network events with per-request ID tracking
  if (method == "Network.requestWillBeSent") {
    const std::string* request_id = params.FindString("requestId");
    const std::string* type = params.FindStringByDottedPath("type");
    if (request_id) {
      // Skip long-running connection types that would stall tracking
      bool skip = false;
      if (type) {
        skip = (*type == "WebSocket" || *type == "EventSource" ||
                *type == "Ping" || *type == "Prefetch" ||
                *type == "CSPViolationReport");
      }
      // Skip third-party requests: only track requests whose host is
      // the page's registrable domain or a subdomain of it.
      if (!skip && !waiter->page_registrable_domain.empty()) {
        const std::string* url_str =
            params.FindStringByDottedPath("request.url");
        if (url_str) {
          GURL request_url(*url_str);
          std::string request_host(request_url.host());
          const std::string& page_domain = waiter->page_registrable_domain;
          // Match: host == domain OR host ends with ".domain"
          bool same_site =
              request_host == page_domain ||
              (request_host.size() > page_domain.size() &&
               request_host.compare(request_host.size() - page_domain.size(),
                                    page_domain.size(), page_domain) == 0 &&
               request_host[request_host.size() - page_domain.size() - 1] ==
                   '.');
          if (!same_site) {
            skip = true;
          }
        }
      }
      if (!skip) {
        waiter->active_request_ids.insert(*request_id);
      }
    }
    waiter->active_requests++;
    waiter->last_network_activity = base::TimeTicks::Now();
    waiter->network_idle = false;
  } else if (method == "Network.loadingFinished" ||
             method == "Network.loadingFailed") {
    const std::string* request_id = params.FindString("requestId");
    if (request_id) {
      waiter->active_request_ids.erase(*request_id);
      waiter->tracked_requests.erase(*request_id);
      // Check if all tracked requests are now resolved
      if (waiter->tracking_snapshot_taken &&
          waiter->tracked_requests.empty() &&
          !waiter->tracked_requests_resolved) {
        waiter->tracked_requests_resolved = true;
        VLOG(1) << "ABP PROFILE [wait] Phase2 tracked requests RESOLVED"
                  << " elapsed=" << (base::TimeTicks::Now() - waiter->action_start_time).InMilliseconds() << "ms"
                  << " tab=" << tab_id;
        // Start Phase 3: post-tracking settle
        if (!waiter->post_tracking_settle_started) {
          waiter->post_tracking_settle_started = true;
          VLOG(1) << "ABP PROFILE [wait] Phase3 settle START"
                    << " settle=" << waiter->post_tracking_settle_time.InMilliseconds() << "ms"
                    << " tab=" << tab_id;
          content::GetUIThreadTaskRunner({})->PostDelayedTask(
              FROM_HERE,
              base::BindOnce(&AbpController::OnPostTrackingSettle,
                             weak_factory_.GetWeakPtr(), tab_id),
              waiter->post_tracking_settle_time);
        }
      }
    }
    if (waiter->active_requests > 0) {
      waiter->active_requests--;
    }
    waiter->last_network_activity = base::TimeTicks::Now();
  }

}

void AbpController::OnPageLifecycleEvent(const std::string& tab_id,
                                          const std::string& event) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();

  if (event == "load") {
    waiter->load_fired = true;
    VLOG(1) << "ABP PROFILE [wait] event=load"
              << " elapsed=" << (base::TimeTicks::Now() - waiter->action_start_time).InMilliseconds() << "ms"
              << " tab=" << tab_id;
    if (waiter->wait_type == "time" && !waiter->time_wait_started &&
        waiter->dom_content_loaded_fired) {
      OnLoadFiredForTimeWait(tab_id);
    }
  } else if (event == "dom_content_loaded") {
    waiter->dom_content_loaded_fired = true;
    VLOG(1) << "ABP PROFILE [wait] event=dom_content_loaded"
              << " elapsed=" << (base::TimeTicks::Now() - waiter->action_start_time).InMilliseconds() << "ms"
              << " tab=" << tab_id;
    if (waiter->wait_type == "time" && !waiter->time_wait_started &&
        waiter->load_fired) {
      OnLoadFiredForTimeWait(tab_id);
    }
  } else if (event == "first_paint") {
    waiter->first_paint_fired = true;
    VLOG(1) << "ABP PROFILE [wait] event=first_paint"
              << " elapsed=" << (base::TimeTicks::Now() - waiter->action_start_time).InMilliseconds() << "ms"
              << " tab=" << tab_id;
  }

  MaybeStartMinWaitTimer(tab_id);
  CheckActionCompleteConditions(tab_id);
}

void AbpController::MaybeStartMinWaitTimer(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();

  // Only for action_complete mode
  if (waiter->wait_type != "action_complete") {
    return;
  }

  // Only start once
  if (waiter->min_wait_timer_started) {
    return;
  }

  // Only start when all three base conditions are met
  if (!waiter->load_fired || !waiter->dom_content_loaded_fired ||
      !waiter->first_paint_fired) {
    return;
  }

  waiter->min_wait_timer_started = true;
  VLOG(1) << "ABP PROFILE [wait] min_wait_timer STARTED"
            << " elapsed=" << (base::TimeTicks::Now() - waiter->action_start_time).InMilliseconds() << "ms"
            << " min_wait=" << waiter->min_wait_time.InMilliseconds() << "ms"
            << " tab=" << tab_id;

  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnMinWaitTimeElapsed,
                     weak_factory_.GetWeakPtr(), tab_id),
      waiter->min_wait_time);
}

void AbpController::OnMinWaitTimeElapsed(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();
  waiter->min_time_elapsed = true;

  // Phase 1 complete — take request tracking snapshot
  if (waiter->all_requests) {
    // browser_wait mode: seed from persistent tracking (all in-flight same-site
    // requests regardless of when they started, not just since wait began).
    waiter->tracked_requests = it->second.persistent_active_request_ids;
    // Also include any new requests seen during prewait
    waiter->tracked_requests.insert(waiter->active_request_ids.begin(),
                                    waiter->active_request_ids.end());
  } else {
    waiter->tracked_requests = waiter->active_request_ids;
  }
  waiter->tracking_snapshot_taken = true;

  VLOG(1) << "ABP PROFILE [wait] min_wait ELAPSED"
            << " elapsed=" << (base::TimeTicks::Now() - waiter->action_start_time).InMilliseconds() << "ms"
            << " active_requests=" << waiter->active_request_ids.size()
            << " tracked=" << waiter->tracked_requests.size()
            << " tab=" << tab_id;

  if (waiter->tracked_requests.empty()) {
    // No requests in flight — resolve immediately, start Phase 3
    waiter->tracked_requests_resolved = true;
    waiter->post_tracking_settle_started = true;
    VLOG(1) << "ABP PROFILE [wait] Phase3 settle START (no tracked requests)"
              << " settle=" << waiter->post_tracking_settle_time.InMilliseconds() << "ms"
              << " tab=" << tab_id;
    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpController::OnPostTrackingSettle,
                       weak_factory_.GetWeakPtr(), tab_id),
        waiter->post_tracking_settle_time);
  } else {
    // Requests in flight — start Phase 2 tracking timeout
    VLOG(1) << "ABP PROFILE [wait] Phase2 tracking START"
              << " tracking " << waiter->tracked_requests.size()
              << " requests, timeout=" << waiter->request_tracking_timeout.InMilliseconds() << "ms"
              << " tab=" << tab_id;
    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpController::OnRequestTrackingTimeout,
                       weak_factory_.GetWeakPtr(), tab_id),
        waiter->request_tracking_timeout);
  }

  CheckActionCompleteConditions(tab_id);
}

void AbpController::OnAnimationWaitTimeElapsed(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();
  waiter->animation_time_elapsed = true;
  VLOG(1) << "ABP PROFILE [wait] animation_timer ELAPSED"
          << " elapsed=" << (base::TimeTicks::Now() - waiter->action_start_time).InMilliseconds() << "ms"
          << " tab=" << tab_id;

  CheckActionCompleteConditions(tab_id);
}

void AbpController::OnRequestTrackingTimeout(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();
  if (waiter->tracked_requests_resolved) {
    return;  // Already resolved normally before timeout
  }

  int unresolved_count = static_cast<int>(waiter->tracked_requests.size());
  VLOG(1) << "ABP PROFILE [wait] Phase2 tracking TIMEOUT"
            << " elapsed=" << (base::TimeTicks::Now() - waiter->action_start_time).InMilliseconds() << "ms"
            << " unresolved=" << unresolved_count
            << " tab=" << tab_id;
  waiter->tracked_requests_resolved = true;
  waiter->tracking_timed_out = true;

  // Generate event so the agent knows tracking timed out
  if (event_collector_ && event_collector_->IsCapturing() &&
      event_collector_->GetCapturingTabId() == tab_id) {
    base::Value::Dict data;
    data.Set("unresolved_requests", unresolved_count);
    event_collector_->AddEvent("request_tracking_timeout", std::move(data));
  }

  // Start Phase 3: post-tracking settle
  if (!waiter->post_tracking_settle_started) {
    waiter->post_tracking_settle_started = true;
    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpController::OnPostTrackingSettle,
                       weak_factory_.GetWeakPtr(), tab_id),
        waiter->post_tracking_settle_time);
  }

  CheckActionCompleteConditions(tab_id);
}

void AbpController::OnPostTrackingSettle(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  VLOG(1) << "ABP PROFILE [wait] Phase3 settle DONE"
            << " elapsed=" << (base::TimeTicks::Now() - it->second.action_waiter->action_start_time).InMilliseconds() << "ms"
            << " tab=" << tab_id;
  it->second.action_waiter->post_tracking_settled = true;
  CheckActionCompleteConditions(tab_id);
}

void AbpController::OnNetworkIdleCheck(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();

  // Check if network has been idle (≤2 connections) for kNetworkIdleTime
  if (waiter->active_requests <= kNetworkIdleMaxConnections) {
    base::TimeDelta idle_duration =
        base::TimeTicks::Now() - waiter->last_network_activity;
    if (idle_duration >= kNetworkIdleTime) {
      waiter->network_idle = true;
      // Network is now idle — if min_wait was deferred, start it now.
      MaybeStartMinWaitTimer(tab_id);
      CheckActionCompleteConditions(tab_id);
      return;
    }
  }

  // Not idle yet, schedule another check
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnNetworkIdleCheck,
                     weak_factory_.GetWeakPtr(), tab_id),
      kNetworkIdleCheckInterval);
}

void AbpController::OnWaitTimeout(const std::string& tab_id,
                                   uint64_t waiter_epoch) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }
  // Ignore stale timeouts from prior waiters
  if (it->second.action_waiter->waiter_epoch != waiter_epoch) {
    return;
  }

  VLOG(1) << "ABP PROFILE [wait] TIMEOUT (10s safety net)"
               << " elapsed=" << (base::TimeTicks::Now() - it->second.action_waiter->action_start_time).InMilliseconds() << "ms"
               << " load=" << it->second.action_waiter->load_fired
               << " dcl=" << it->second.action_waiter->dom_content_loaded_fired
               << " paint=" << it->second.action_waiter->first_paint_fired
               << " min_elapsed=" << it->second.action_waiter->min_time_elapsed
               << " tracked_resolved=" << it->second.action_waiter->tracked_requests_resolved
               << " post_settled=" << it->second.action_waiter->post_tracking_settled
               << " anim_elapsed=" << it->second.action_waiter->animation_time_elapsed
               << " tab=" << tab_id;

  // Force complete - removing the waiter stops OnCdpEventForWait from processing events
  std::unique_ptr<ActionCompleteWaiter> waiter = std::move(it->second.action_waiter);

  if (waiter->on_complete) {
    std::move(waiter->on_complete).Run();
  }
}

void AbpController::OnPersistentNetworkEvent(const std::string& tab_id,
                                              const std::string& method,
                                              const base::Value::Dict& params) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) return;
  TabState& tab_state = it->second;

  // On main-frame navigation: update domain and clear stale requests.
  if (method == "Page.frameNavigated") {
    const base::Value::Dict* frame = params.FindDict("frame");
    if (frame && !frame->FindString("parentId")) {  // main frame only
      const std::string* url_str = frame->FindString("url");
      if (url_str) {
        GURL url(*url_str);
        tab_state.persistent_page_domain =
            net::registry_controlled_domains::GetDomainAndRegistry(
                url,
                net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
      }
      tab_state.persistent_active_request_ids.clear();
    }
    return;
  }

  if (method == "Network.requestWillBeSent") {
    const std::string* request_id = params.FindString("requestId");
    if (!request_id) return;

    // Skip long-running connection types
    const std::string* type = params.FindStringByDottedPath("type");
    if (type && (*type == "WebSocket" || *type == "EventSource" ||
                 *type == "Ping" || *type == "Prefetch" ||
                 *type == "CSPViolationReport")) {
      return;
    }

    // Apply same-site domain filter
    if (!tab_state.persistent_page_domain.empty()) {
      const std::string* url_str = params.FindStringByDottedPath("request.url");
      if (url_str) {
        GURL url(*url_str);
        const std::string& domain = tab_state.persistent_page_domain;
        std::string host(url.host());
        bool same_site =
            (host == domain ||
             (host.size() > domain.size() &&
              host.compare(host.size() - domain.size(), domain.size(),
                           domain) == 0 &&
              host[host.size() - domain.size() - 1] == '.'));
        if (!same_site) return;
      }
    }
    tab_state.persistent_active_request_ids.insert(*request_id);

  } else if (method == "Network.loadingFinished" ||
             method == "Network.loadingFailed") {
    const std::string* request_id = params.FindString("requestId");
    if (request_id) {
      tab_state.persistent_active_request_ids.erase(*request_id);
    }
  }

  // Forward to per-tab network capture buffer (always-on).
  if (tab_state.network_capture) {
    tab_state.network_capture->OnNetworkEvent(method, params,
                                              tab_state.cdp_client.get());
  }
}

void AbpController::CheckActionCompleteConditions(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();

  if (!waiter->IsComplete()) {
    return;
  }

  // All conditions met - removing the waiter stops OnCdpEventForWait from processing events
  std::unique_ptr<ActionCompleteWaiter> completed_waiter = std::move(it->second.action_waiter);

  VLOG(1) << "ABP PROFILE [wait] complete"
            << " elapsed=" << (base::TimeTicks::Now() - completed_waiter->action_start_time).InMilliseconds() << "ms"
            << " load=" << completed_waiter->load_fired
            << " dcl=" << completed_waiter->dom_content_loaded_fired
            << " paint=" << completed_waiter->first_paint_fired
            << " tracking_timed_out=" << completed_waiter->tracking_timed_out
            << " tab=" << tab_id;

  if (completed_waiter->on_complete) {
    std::move(completed_waiter->on_complete).Run();
  }
}

void AbpController::WaitFor(const std::string& tab_id,
                            const base::Value::Dict& wait_params,
                            base::OnceClosure on_complete) {
  const std::string* type = wait_params.FindString("type");
  if (!type) {
    // No type specified, fall through to immediate completion
    std::move(on_complete).Run();
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(on_complete).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(on_complete).Run();
    return;
  }

  auto waiter = std::make_unique<ActionCompleteWaiter>();
  waiter->tab_id = tab_id;
  waiter->action_start_time = base::TimeTicks::Now();
  waiter->on_complete = std::move(on_complete);
  waiter->timeout_time = base::TimeTicks::Now() + kWaitTimeout;
  waiter->wait_type = *type;

  if (*type == "text") {
    const std::string* text = wait_params.FindString("text");
    if (!text || text->empty()) {
      std::move(waiter->on_complete).Run();
      return;
    }
    waiter->wait_text = *text;
  } else if (*type == "url") {
    const std::string* pattern = wait_params.FindString("url");
    if (!pattern || pattern->empty()) {
      std::move(waiter->on_complete).Run();
      return;
    }
    waiter->wait_url_pattern = *pattern;
  } else if (*type == "time") {
    auto ms = wait_params.FindInt("ms");
    if (!ms || *ms <= 0) {
      std::move(waiter->on_complete).Run();
      return;
    }
    waiter->time_wait_ms = *ms;

    // If page is already loaded, time wait starts immediately
    if (!wc->IsLoading()) {
      waiter->load_fired = true;
      waiter->dom_content_loaded_fired = true;
    }
  } else if (*type == "network_idle") {
    waiter->last_network_activity = base::TimeTicks::Now();
  } else {
    // Unknown type, complete immediately
    std::move(waiter->on_complete).Run();
    return;
  }

  GetOrCreateTabState(tab_id).action_waiter = std::move(waiter);

  // Enable Network and Page domains for events
  base::Value::Dict empty_params;
  client->SendCommand("Network.enable", empty_params,
                      base::BindOnce([](bool, const std::string&) {}));
  base::Value::Dict empty_params2;
  client->SendCommand("Page.enable", empty_params2,
                      base::BindOnce([](bool, const std::string&) {}));

  // Start type-specific polling
  if (*type == "text") {
    OnTextPollCheck(tab_id);
  } else if (*type == "url") {
    OnUrlPollCheck(tab_id);
  } else if (*type == "network_idle") {
    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpController::OnNetworkIdleCheck,
                       weak_factory_.GetWeakPtr(), tab_id),
        kNetworkIdleCheckInterval);
  } else if (*type == "time") {
    // For time wait, check if page already loaded to start the timer
    auto it = tab_states_.find(tab_id);
    if (it != tab_states_.end() && it->second.action_waiter) {
      ActionCompleteWaiter* w = it->second.action_waiter.get();
      if (w->load_fired && w->dom_content_loaded_fired) {
        OnLoadFiredForTimeWait(tab_id);
      }
    }
  }

  // Start timeout timer (pass epoch for stale-timeout filtering)
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnWaitTimeout,
                     weak_factory_.GetWeakPtr(), tab_id,
                     GetOrCreateTabState(tab_id).action_waiter->waiter_epoch),
      kWaitTimeout);
}

void AbpController::OnTextPollCheck(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();
  if (waiter->wait_type != "text" || waiter->text_found) {
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    return;
  }

  // Escape special characters in the search text for JS
  std::string escaped_text = waiter->wait_text;
  base::ReplaceSubstringsAfterOffset(&escaped_text, 0, "\\", "\\\\");
  base::ReplaceSubstringsAfterOffset(&escaped_text, 0, "'", "\\'");
  base::ReplaceSubstringsAfterOffset(&escaped_text, 0, "\n", "\\n");
  base::ReplaceSubstringsAfterOffset(&escaped_text, 0, "\r", "\\r");

  std::string expression =
      "document.body && document.body.innerText.includes('" + escaped_text + "')";

  base::Value::Dict cdp_params;
  cdp_params.Set("expression", expression);
  cdp_params.Set("returnByValue", true);
  cdp_params.Set("disableBreaks", true);

  client->SendCommand(
      "Runtime.evaluate", cdp_params,
      base::BindOnce(&AbpController::OnTextPollResult,
                     weak_factory_.GetWeakPtr(), tab_id));
}

void AbpController::OnTextPollResult(const std::string& tab_id,
                                     bool success,
                                     const std::string& result) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();

  if (success) {
    auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
    if (parsed && parsed->is_dict()) {
      const base::Value::Dict* cdp_result = parsed->GetDict().FindDict("result");
      if (cdp_result) {
        auto value = cdp_result->FindBool("value");
        if (value && *value) {
          waiter->text_found = true;
          CheckActionCompleteConditions(tab_id);
          return;
        }
      }
    }
  }

  // Not found yet, poll again after 200ms
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnTextPollCheck,
                     weak_factory_.GetWeakPtr(), tab_id),
      base::Milliseconds(200));
}

void AbpController::OnUrlPollCheck(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();
  if (waiter->wait_type != "url" || waiter->url_matched) {
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    return;
  }

  std::string current_url = wc->GetVisibleURL().spec();
  if (current_url.find(waiter->wait_url_pattern) != std::string::npos) {
    waiter->url_matched = true;
    CheckActionCompleteConditions(tab_id);
    return;
  }

  // Not matched yet, poll again after 100ms
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnUrlPollCheck,
                     weak_factory_.GetWeakPtr(), tab_id),
      base::Milliseconds(100));
}

void AbpController::OnLoadFiredForTimeWait(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();
  if (waiter->time_wait_started) {
    return;
  }

  waiter->time_wait_started = true;

  // Start the deferred timer
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<AbpController> controller, std::string tab_id) {
            if (!controller) return;
            auto it = controller->tab_states_.find(tab_id);
            if (it == controller->tab_states_.end() || !it->second.action_waiter) {
              return;
            }
            it->second.action_waiter->time_wait_elapsed = true;
            controller->CheckActionCompleteConditions(tab_id);
          },
          weak_factory_.GetWeakPtr(), tab_id),
      base::Milliseconds(waiter->time_wait_ms));
}

int64_t AbpController::GetVirtualTimeMs(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end() && it->second.execution.IsEnabled()) {
    return static_cast<int64_t>(it->second.execution.virtual_time_base_ticks_ms);
  }
  return base::Time::Now().InMillisecondsSinceUnixEpoch();
}

void AbpController::OnFileChooserOpened(const std::string& chooser_id,
                                        const std::string& tab_id,
                                        base::Value::Dict info) {
  pending_file_choosers_[chooser_id] = std::move(info);
  VLOG(1) << "ABP: File chooser opened with ID " << chooser_id
            << " for tab " << tab_id;
}

void AbpController::GetScrollPosition(
    const std::string& tab_id,
    base::OnceCallback<void(base::Value::Dict)> callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    base::Value::Dict empty;
    std::move(callback).Run(std::move(empty));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    base::Value::Dict empty;
    std::move(callback).Run(std::move(empty));
    return;
  }

  // Execute JavaScript to get scroll info
  VLOG(1) << "ABP PROFILE [scroll] GetScrollPosition SEND tab=" << tab_id;
  auto scroll_send = base::TimeTicks::Now();
  const std::string script = R"(
    (function() {
      return {
        scrollX: window.scrollX || window.pageXOffset || 0,
        scrollY: window.scrollY || window.pageYOffset || 0,
        pageWidth: Math.max(
          document.body.scrollWidth || 0,
          document.documentElement.scrollWidth || 0
        ),
        pageHeight: Math.max(
          document.body.scrollHeight || 0,
          document.documentElement.scrollHeight || 0
        ),
        viewportWidth: window.innerWidth || 0,
        viewportHeight: window.innerHeight || 0
      };
    })()
  )";

  base::Value::Dict eval_params;
  eval_params.Set("expression", script);
  eval_params.Set("returnByValue", true);
  eval_params.Set("disableBreaks", true);

  // Use shared state so a safety-net timeout can fire the callback if
  // Runtime.evaluate hangs (e.g. after cross-process navigation to a heavy
  // page where the CDP session may be suspended).
  auto done = std::make_shared<bool>(false);
  auto shared_cb =
      std::make_shared<base::OnceCallback<void(base::Value::Dict)>>(
          std::move(callback));

  // 5s safety timeout — return empty scroll info rather than hang forever.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<bool> d,
             std::shared_ptr<base::OnceCallback<void(base::Value::Dict)>> cb) {
            if (*d) return;
            *d = true;
            LOG(WARNING) << "ABP: GetScrollPosition timed out (5s)";
            std::move(*cb).Run(base::Value::Dict());
          },
          done, shared_cb),
      base::Seconds(5));

  client->SendCommand(
      "Runtime.evaluate", std::move(eval_params),
      base::BindOnce(
          [](std::shared_ptr<bool> d,
             std::shared_ptr<base::OnceCallback<void(base::Value::Dict)>> cb,
             base::TimeTicks send_time,
             bool success, const std::string& result) {
            if (*d) return;
            *d = true;
            VLOG(1) << "ABP PROFILE [scroll] GetScrollPosition DONE"
                      << " elapsed=" << (base::TimeTicks::Now() - send_time).InMilliseconds() << "ms";
            base::Value::Dict scroll_info;

            if (success) {
              auto parsed = base::JSONReader::Read(
                  result, base::JSON_ALLOW_TRAILING_COMMAS);
              if (parsed && parsed->is_dict()) {
                const base::Value::Dict* result_obj =
                    parsed->GetDict().FindDict("result");
                if (result_obj) {
                  const base::Value::Dict* value =
                      result_obj->FindDict("value");
                  if (value) {
                    double scrollX = value->FindDouble("scrollX").value_or(0);
                    double scrollY = value->FindDouble("scrollY").value_or(0);
                    double pageWidth = value->FindDouble("pageWidth").value_or(0);
                    double pageHeight = value->FindDouble("pageHeight").value_or(0);
                    double viewportWidth = value->FindDouble("viewportWidth").value_or(0);
                    double viewportHeight = value->FindDouble("viewportHeight").value_or(0);

                    scroll_info.Set("horizontal_px", static_cast<int>(scrollX));
                    scroll_info.Set("vertical_px", static_cast<int>(scrollY));
                    scroll_info.Set("page_width", static_cast<int>(pageWidth));
                    scroll_info.Set("page_height", static_cast<int>(pageHeight));
                    scroll_info.Set("viewport_width", static_cast<int>(viewportWidth));
                    scroll_info.Set("viewport_height", static_cast<int>(viewportHeight));

                    // Calculate percentages
                    double h_percent = 0;
                    double v_percent = 0;
                    if (pageWidth > viewportWidth) {
                      h_percent = (scrollX / (pageWidth - viewportWidth)) * 100.0;
                    }
                    if (pageHeight > viewportHeight) {
                      v_percent = (scrollY / (pageHeight - viewportHeight)) * 100.0;
                    }
                    scroll_info.Set("horizontal_percent", h_percent);
                    scroll_info.Set("vertical_percent", v_percent);
                  }
                }
              }
            }

            std::move(*cb).Run(std::move(scroll_info));
          },
          done, shared_cb, scroll_send));
}

void AbpController::CaptureScreenshotBase64(
    const std::string& tab_id,
    base::OnceCallback<void(std::string base64, int width, int height)> callback) {
  VLOG(1) << "ABP: CaptureScreenshotBase64 tab=" << tab_id;

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run(std::string(), 0, 0);
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    LOG(WARNING) << "ABP: CaptureScreenshotBase64 - no RenderWidgetHostView";
    std::move(callback).Run(std::string(), 0, 0);
    return;
  }

  // Use ForceRedrawWithCallback + GrabViewSnapshot instead of CDP
  // Page.captureScreenshot which can hang indefinitely.
  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      view->GetRenderWidgetHost());

  struct Base64SnapState {
    bool done = false;
    base::OnceCallback<void(std::string, int, int)> cb;
    std::string tab_id;
    int vp_width = 0;
    int vp_height = 0;
  };
  auto st = std::make_shared<Base64SnapState>();
  st->cb = std::move(callback);
  st->tab_id = tab_id;
  gfx::Size vp_size = view->GetVisibleViewportSize();
  st->vp_width = vp_size.width();
  st->vp_height = vp_size.height();

  // 1500ms safety timeout
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<Base64SnapState> s) {
            if (s->done) return;
            s->done = true;
            LOG(WARNING) << "ABP: CaptureScreenshotBase64 timed out";
            std::move(s->cb).Run(std::string(), 0, 0);
          },
          st),
      base::Milliseconds(1500));

  VLOG(1) << "ABP: CaptureScreenshotBase64 - calling ForceRedrawWithCallback";
  rwhi->ForceRedrawWithCallback(base::BindOnce(
      [](std::shared_ptr<Base64SnapState> s,
         base::WeakPtr<AbpController> ctrl) {
        if (s->done) return;
        VLOG(1) << "ABP: CaptureScreenshotBase64 - ForceRedraw callback fired";
        if (!ctrl) {
          s->done = true;
          std::move(s->cb).Run(std::string(), 0, 0);
          return;
        }
        // 50ms CoreAnimation delay
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(
                [](std::shared_ptr<Base64SnapState> s,
                   base::WeakPtr<AbpController> ctrl) {
                  if (s->done) return;
                  if (!ctrl) {
                    s->done = true;
                    std::move(s->cb).Run(std::string(), 0, 0);
                    return;
                  }
                  content::WebContents* wc2 =
                      ctrl->FindWebContents(s->tab_id);
                  if (!wc2) {
                    s->done = true;
                    std::move(s->cb).Run(std::string(), 0, 0);
                    return;
                  }
                  gfx::NativeView native_view = wc2->GetContentNativeView();
                  if (!native_view) {
                    s->done = true;
                    std::move(s->cb).Run(std::string(), 0, 0);
                    return;
                  }
                  auto* view2 = wc2->GetRenderWidgetHostView();
                  gfx::Rect bounds;
                  if (view2) {
                    bounds = gfx::Rect(view2->GetViewBounds().size());
                  }
                  ui::GrabViewSnapshot(
                      native_view, bounds,
                      base::BindOnce(
                          [](std::shared_ptr<Base64SnapState> s,
                             gfx::Image image) {
                            if (s->done) return;
                            s->done = true;
                            if (image.IsEmpty()) {
                              LOG(WARNING) << "ABP: CaptureScreenshotBase64"
                                           << " - GrabViewSnapshot empty";
                              std::move(s->cb).Run(std::string(), 0, 0);
                              return;
                            }
                            VLOG(1) << "ABP: CaptureScreenshotBase64"
                                      << " - encoding WebP";
                            const SkBitmap& raw = *image.ToSkBitmap();
                            const SkBitmap bitmap = ScaleBitmapToViewport(
                                raw, s->vp_width, s->vp_height);
                            auto encoded =
                                gfx::WebpCodec::Encode(bitmap, 80);
                            if (!encoded || encoded->empty()) {
                              LOG(WARNING)
                                  << "ABP: CaptureScreenshotBase64"
                                  << " - WebP encode failed";
                              std::move(s->cb).Run(std::string(), 0, 0);
                              return;
                            }
                            std::string b64 = base::Base64Encode(*encoded);
                            VLOG(1) << "ABP: CaptureScreenshotBase64"
                                      << " - success "
                                      << bitmap.width() << "x"
                                      << bitmap.height();
                            std::move(s->cb).Run(
                                std::move(b64),
                                bitmap.width(), bitmap.height());
                          },
                          s));
                },
                s, ctrl),
            kCoreAnimationDelay);
      },
      st, weak_factory_.GetWeakPtr()));
}

// Dialog endpoints

void AbpController::GetDialog(const std::string& tab_id,
                              ResponseCallback callback) {
  auto it = tab_states_.find(tab_id);

  base::Value::Dict response;
  if (it != tab_states_.end() && it->second.pending_dialog.has_value()) {
    const PendingDialog& dialog = it->second.pending_dialog.value();
    response.Set("present", true);
    response.Set("dialog_type", dialog.dialog_type);
    response.Set("message", dialog.message);
    if (!dialog.default_prompt.empty()) {
      response.Set("default_prompt", dialog.default_prompt);
    }
  } else {
    response.Set("present", false);
  }

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::AcceptDialog(const std::string& tab_id,
                                 const base::Value::Dict& params,
                                 ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.pending_dialog.has_value()) {
    SendError(400, "No pending dialog", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to get CDP client", std::move(callback));
    return;
  }

  base::Value::Dict cdp_params;
  cdp_params.Set("accept", true);

  // Include prompt text if provided (for prompt dialogs)
  const std::string* prompt_text = params.FindString("prompt_text");
  if (prompt_text) {
    cdp_params.Set("promptText", *prompt_text);
  }

  client->SendCommand(
      "Page.handleJavaScriptDialog", std::move(cdp_params),
      base::BindOnce(
          [](base::WeakPtr<AbpController> weak_this, std::string tab_id,
             ResponseCallback cb, bool success, const std::string& result) {
            if (!weak_this) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"Controller destroyed"})");
              return;
            }
            if (!success) {
              weak_this->SendError(500, "Failed to handle dialog",
                                   std::move(cb));
              return;
            }
            // Remove from pending
            auto state_it = weak_this->tab_states_.find(tab_id);
            if (state_it != weak_this->tab_states_.end()) {
              state_it->second.pending_dialog.reset();
            }

            base::Value::Dict response;
            response.Set("success", true);
            weak_this->SendJson(200, base::Value(std::move(response)),
                               std::move(cb));
          },
          weak_factory_.GetWeakPtr(), tab_id, std::move(callback)));
}

void AbpController::DismissDialog(const std::string& tab_id,
                                  ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.pending_dialog.has_value()) {
    SendError(400, "No pending dialog", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to get CDP client", std::move(callback));
    return;
  }

  base::Value::Dict cdp_params;
  cdp_params.Set("accept", false);

  client->SendCommand(
      "Page.handleJavaScriptDialog", std::move(cdp_params),
      base::BindOnce(
          [](base::WeakPtr<AbpController> weak_this, std::string tab_id,
             ResponseCallback cb, bool success, const std::string& result) {
            if (!weak_this) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"Controller destroyed"})");
              return;
            }
            if (!success) {
              weak_this->SendError(500, "Failed to dismiss dialog",
                                   std::move(cb));
              return;
            }
            // Remove from pending
            auto state_it = weak_this->tab_states_.find(tab_id);
            if (state_it != weak_this->tab_states_.end()) {
              state_it->second.pending_dialog.reset();
            }

            base::Value::Dict response;
            response.Set("success", true);
            weak_this->SendJson(200, base::Value(std::move(response)),
                               std::move(cb));
          },
          weak_factory_.GetWeakPtr(), tab_id, std::move(callback)));
}

void AbpController::OnDialogOpened(const std::string& tab_id,
                                   const std::string& dialog_type,
                                   const std::string& message,
                                   const std::string& default_prompt) {
  PendingDialog dialog;
  dialog.dialog_type = dialog_type;
  dialog.message = message;
  dialog.default_prompt = default_prompt;
  dialog.opened_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  GetOrCreateTabState(tab_id).pending_dialog = std::move(dialog);
  VLOG(1) << "ABP: Dialog opened in tab " << tab_id << " type=" << dialog_type;
}

void AbpController::OnDialogClosed(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end()) {
    it->second.pending_dialog.reset();
  }
  VLOG(1) << "ABP: Dialog closed in tab " << tab_id;
}

// File chooser endpoint

void AbpController::HandleFileChooser(const std::string& chooser_id,
                                      const base::Value::Dict& params,
                                      ResponseCallback callback) {
  auto it = pending_file_choosers_.find(chooser_id);
  if (it == pending_file_choosers_.end()) {
    SendError(404, "File chooser not found", std::move(callback));
    return;
  }

  const std::string* tab_id = it->second.FindString("tab_id");
  if (!tab_id) {
    SendError(500, "Invalid file chooser state", std::move(callback));
    return;
  }

  content::WebContents* wc = FindWebContents(*tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to get CDP client", std::move(callback));
    return;
  }

  // Check if cancel is requested
  auto cancel = params.FindBool("cancel");
  if (cancel && *cancel) {
    pending_file_choosers_.erase(chooser_id);
    base::Value::Dict response;
    response.Set("success", true);
    response.Set("cancelled", true);
    SendJson(200, base::Value(std::move(response)), std::move(callback));
    return;
  }

  // Get backendNodeId from stored chooser info (needed for DOM.setFileInputFiles)
  auto backend_node_id = it->second.FindInt("backendNodeId");
  if (!backend_node_id) {
    SendError(500, "File chooser missing backendNodeId (not an <input> element?)",
              std::move(callback));
    return;
  }

  // Get files to provide
  const base::Value::List* files = params.FindList("files");
  const std::string* save_path = params.FindString("path");

  // Check for content_files (base64-encoded file data)
  const base::Value::List* content_files = params.FindList("content_files");

  if (!files && !save_path && !content_files) {
    SendError(400,
              "Must provide 'files' array, 'content_files' array, "
              "or 'path' for save dialog",
              std::move(callback));
    return;
  }

  // Parse max_size (default 10MB)
  int64_t max_size = 10 * 1024 * 1024;
  if (auto ms = params.FindDouble("max_size")) {
    max_size = static_cast<int64_t>(*ms);
  }

  int node_id = *backend_node_id;
  std::string tab_id_str = *tab_id;
  std::string chooser_id_copy = chooser_id;

  // If content_files present, decode and write to temp files first
  if (content_files && !content_files->empty()) {
    // Validate and decode all content_files upfront
    std::vector<std::pair<std::string, std::vector<uint8_t>>> pending;

    for (size_t i = 0; i < content_files->size(); i++) {
      if (!(*content_files)[i].is_dict()) {
        SendError(400, "content_files[" + base::NumberToString(i) +
                           "]: must be an object",
                  std::move(callback));
        return;
      }
      const base::Value::Dict& cf = (*content_files)[i].GetDict();

      const std::string* fn = cf.FindString("filename");
      if (!fn || fn->empty()) {
        SendError(400, "content_files[" + base::NumberToString(i) +
                           "]: missing 'filename'",
                  std::move(callback));
        return;
      }

      const std::string* data_b64 = cf.FindString("data");
      if (!data_b64 || data_b64->empty()) {
        SendError(400, "content_files[" + base::NumberToString(i) +
                           "]: missing 'data'",
                  std::move(callback));
        return;
      }

      std::optional<std::vector<uint8_t>> decoded =
          base::Base64Decode(*data_b64);
      if (!decoded) {
        SendError(400, "content_files[" + base::NumberToString(i) +
                           "]: invalid base64 data",
                  std::move(callback));
        return;
      }

      if (static_cast<int64_t>(decoded->size()) > max_size) {
        SendError(413,
                  "content_files[" + base::NumberToString(i) +
                      "]: file too large (" +
                      base::NumberToString(decoded->size()) +
                      " bytes, max " + base::NumberToString(max_size) + ")",
                  std::move(callback));
        return;
      }

      pending.emplace_back(*fn, std::move(*decoded));
    }

    // Collect existing file paths
    std::vector<std::string> existing_paths;
    if (files) {
      for (const auto& file : *files) {
        if (file.is_string()) {
          existing_paths.push_back(file.GetString());
        }
      }
    }

    // Write temp files on thread pool, then run the action
    base::FilePath uploads_dir = session_dir_.AppendASCII("uploads");

    auto task_runner = base::ThreadPool::CreateTaskRunner(
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE});
    task_runner->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(
            [](base::FilePath uploads_dir,
               std::vector<std::pair<std::string, std::vector<uint8_t>>>
                   pending) -> std::optional<std::vector<std::string>> {
              if (!base::CreateDirectory(uploads_dir))
                return std::nullopt;

              std::vector<std::string> paths;
              for (const auto& pf : pending) {
                std::string unique_name =
                    base::NumberToString(
                        base::Time::Now().InMillisecondsSinceUnixEpoch()) +
                    "_" + pf.first;
                base::FilePath dest = uploads_dir.AppendASCII(unique_name);
                if (!base::WriteFile(dest, pf.second))
                  return std::nullopt;
                paths.push_back(dest.AsUTF8Unsafe());
              }
              return paths;
            },
            uploads_dir, std::move(pending)),
        base::BindOnce(
            [](base::WeakPtr<AbpController> self, std::string chooser_id,
               std::string tab_id, int node_id,
               std::vector<std::string> existing_paths,
               ResponseCallback cb,
               std::optional<std::vector<std::string>> temp_paths) {
              if (!self) {
                std::move(cb).Run(500, "application/json",
                                 R"({"error":"Controller destroyed"})");
                return;
              }

              if (!temp_paths) {
                self->SendError(500, "Failed to write temp files",
                                std::move(cb));
                return;
              }

              // Merge paths: existing + temp
              std::vector<std::string> all_paths = std::move(existing_paths);
              all_paths.insert(all_paths.end(),
                               temp_paths->begin(), temp_paths->end());

              self->RunFileChooserAction(
                  tab_id, chooser_id, node_id,
                  std::move(all_paths), std::move(cb));
            },
            weak_factory_.GetWeakPtr(), chooser_id_copy, tab_id_str, node_id,
            std::move(existing_paths), std::move(callback)));
    return;
  }

  // Path-based flow: collect paths and run the action directly
  std::vector<std::string> all_paths;
  if (files) {
    for (const auto& file : *files) {
      if (file.is_string()) {
        all_paths.push_back(file.GetString());
      }
    }
  } else if (save_path) {
    all_paths.push_back(*save_path);
  }

  RunFileChooserAction(tab_id_str, chooser_id_copy, node_id,
                       std::move(all_paths), std::move(callback));
}

void AbpController::RunFileChooserAction(
    const std::string& tab_id,
    const std::string& chooser_id,
    int backend_node_id,
    std::vector<std::string> file_paths,
    ResponseCallback callback) {
  // File upload is a full ABP action: resume → set files → dispatch change →
  // wait for upload network traffic to settle → wait 2s for page JS to
  // process → pause → screenshot.
  auto opts = GetDefaultActionOptions();
  opts.min_wait_time = base::Milliseconds(500);
  opts.request_tracking_timeout = base::Seconds(60);

  // Build a params dict for the action context (used for history/response)
  base::Value::Dict action_params;
  action_params.Set("chooser_id", chooser_id);
  base::Value::List path_list;
  for (const auto& p : file_paths) {
    path_list.Append(p);
  }
  action_params.Set("files", std::move(path_list));

  AbpActionContext::RunWithOptions(
      this, tab_id, "file_chooser", action_params, opts,
      base::BindOnce(
          [](std::string chooser_id, int node_id,
             std::vector<std::string> paths, AbpActionContext* ctx) {
            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            if (!ctx->controller()) {
              ctx_ref->OnActionError("CONTROLLER_DESTROYED",
                                     "Controller destroyed");
              return;
            }

            std::string tab_id = ctx->tab_id();
            content::WebContents* wc =
                ctx->controller()->FindWebContents(tab_id);
            if (!wc) {
              ctx_ref->OnActionError("TAB_NOT_FOUND", "Tab not found");
              return;
            }
            AbpCdpClient* client =
                ctx->controller()->GetOrCreateCdpClient(wc);
            if (!client) {
              ctx_ref->OnActionError("CDP_ERROR",
                                     "Failed to get CDP client");
              return;
            }

            // Build CDP params
            base::Value::Dict cdp_params;
            base::Value::List file_list;
            for (const auto& p : paths) {
              file_list.Append(p);
            }
            cdp_params.Set("files", std::move(file_list));
            cdp_params.Set("backendNodeId", node_id);

            // Set files via CDP. DOM.setFileInputFiles internally calls
            // SetFilesAndDispatchEvents() which dispatches trusted 'input'
            // and 'change' events (isTrusted: true) — no synthetic event
            // dispatch needed.
            client->SendCommand(
                "DOM.setFileInputFiles", std::move(cdp_params),
                base::BindOnce(
                    [](scoped_refptr<AbpActionContext> ctx,
                       std::string chooser_id,
                       bool success, const std::string& result) {
                      if (ctx->controller())
                        ctx->controller()->pending_file_choosers_.erase(
                            chooser_id);

                      if (!success || !ctx->controller()) {
                        ctx->OnActionError("CDP_ERROR",
                                           "Failed to set files: " + result);
                        return;
                      }

                      base::Value::Dict res;
                      res.Set("success", true);
                      ctx->SetResult(std::move(res));
                      ctx->OnActionDispatched();
                    },
                    ctx_ref, chooser_id));
          },
          chooser_id, backend_node_id, std::move(file_paths)),
      std::move(callback));
}

void AbpController::HandleSelectPopup(const std::string& popup_id,
                                      const base::Value::Dict& params,
                                      ResponseCallback callback) {
  if (!popup_interceptor_) {
    SendError(500, "Popup interceptor not initialized", std::move(callback));
    return;
  }

  // Check if cancel requested
  if (params.FindBool("cancel").value_or(false)) {
    if (popup_interceptor_->CancelSelectPopup(popup_id)) {
      base::Value::Dict result;
      result.Set("success", true);
      result.Set("cancelled", true);
      SendJson(200, base::Value(std::move(result)), std::move(callback));
    } else {
      SendError(404, "Select popup not found: " + popup_id,
                std::move(callback));
    }
    return;
  }

  // Get indices
  const base::Value::List* indices_list = params.FindList("indices");
  if (!indices_list || indices_list->empty()) {
    SendError(400, "Missing or empty 'indices' array", std::move(callback));
    return;
  }

  std::vector<int32_t> indices;
  for (const auto& v : *indices_list) {
    if (!v.is_int()) {
      SendError(400, "indices must contain integers", std::move(callback));
      return;
    }
    indices.push_back(v.GetInt());
  }

  // Validate popup exists
  auto popup_info = popup_interceptor_->GetPendingSelectPopup(popup_id);
  if (popup_info.empty()) {
    SendError(404, "Select popup not found: " + popup_id,
              std::move(callback));
    return;
  }

  // Send the selection
  if (!popup_interceptor_->RespondToSelectPopup(popup_id, indices)) {
    SendError(500, "Failed to respond to select popup", std::move(callback));
    return;
  }

  base::Value::Dict result;
  result.Set("success", true);
  result.Set("tab_id", *popup_info.FindString("tab_id"));
  SendJson(200, base::Value(std::move(result)), std::move(callback));
}

void AbpController::HandleDateTimePopup(const std::string& popup_id,
                                        const base::Value::Dict& params,
                                        ResponseCallback callback) {
  if (!popup_interceptor_) {
    SendError(500, "Popup interceptor not initialized", std::move(callback));
    return;
  }

  // Check if cancel requested
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

  // Validate popup exists
  auto popup_info = popup_interceptor_->GetPendingDateTimePopup(popup_id);
  if (popup_info.empty()) {
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
  result.Set("tab_id", *popup_info.FindString("tab_id"));
  SendJson(200, base::Value(std::move(result)), std::move(callback));
}

// Binary screenshot endpoint (GET)

void AbpController::BinaryScreenshot(const std::string& tab_id,
                                     const std::string& query,
                                     ResponseCallback callback) {
  VLOG(1) << "ABP: BinaryScreenshot tab=" << tab_id;
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    SendError(500, "No render view available", std::move(callback));
    return;
  }

  // Parse query params for markup tags.
  // New API: ?markup=clickable,grid — enable specific overlays (none by default).
  // Legacy: ?disable_markup=grid — all overlays minus disabled ones.
  // In CDP mode, markup is skipped (CSS injection requires CDP clients which are detached).
  std::vector<std::string> markup_tags;
  if (!query.empty() && input_mode_ != InputMode::kCdp) {
    auto parse_csv = [](const std::string& q, const std::string& key)
        -> std::vector<std::string> {
      std::vector<std::string> result;
      size_t pos = q.find(key + "=");
      if (pos == std::string::npos) return result;
      size_t start = pos + key.size() + 1;
      size_t end = q.find('&', start);
      std::string val = q.substr(start, end == std::string::npos ? end : end - start);
      size_t i = 0;
      while (i < val.size()) {
        size_t comma = val.find(',', i);
        if (comma == std::string::npos) comma = val.size();
        std::string tag = val.substr(i, comma - i);
        if (!tag.empty()) result.push_back(tag);
        i = comma + 1;
      }
      return result;
    };

    markup_tags = parse_csv(query, "markup");
    if (markup_tags.empty()) {
      // Legacy: disable_markup = all minus disabled
      auto disable_tags = parse_csv(query, "disable_markup");
      if (!disable_tags.empty()) {
        std::set<std::string> disabled(disable_tags.begin(), disable_tags.end());
        for (const char* tag : kAllMarkupTags) {
          if (disabled.find(tag) == disabled.end()) {
            markup_tags.emplace_back(tag);
          }
        }
      }
    }
  }

  // Validate markup tags
  std::string invalid_tag;
  if (!ValidateMarkupTags(markup_tags, &invalid_tag)) {
    SendError(400, "Unknown markup tag: " + invalid_tag, std::move(callback));
    return;
  }

  bool restore_pause_after_capture = false;
  auto tab_it = tab_states_.find(tab_id);
  if (tab_it != tab_states_.end()) {
    const auto& exec = tab_it->second.execution;
    restore_pause_after_capture = exec.IsPaused();
  }

  auto wrapped_cb = base::BindOnce(
      [](base::WeakPtr<AbpController> ctrl, std::string tid,
         bool restore_pause,
         ResponseCallback orig,
         int status, const std::string& ct, std::string body) {
        if (!ctrl || !restore_pause) {
          std::move(orig).Run(status, ct, std::move(body));
          return;
        }
        ctrl->RestoreVirtualTimePause(
            tid,
            base::BindOnce(
                [](ResponseCallback cb, int s, std::string ct, std::string b) {
                  std::move(cb).Run(s, std::move(ct), std::move(b));
                },
                std::move(orig), status, std::string(ct), std::move(body)));
      },
      weak_factory_.GetWeakPtr(), tab_id, restore_pause_after_capture,
      std::move(callback));

  if (!markup_tags.empty()) {
    // Inject markup, ForceRedraw, capture, cleanup
    AbpCdpClient* client = GetOrCreateCdpClient(wc);
    if (!client) {
      SendError(500, "No CDP client", std::move(wrapped_cb));
      return;
    }
    std::string inject_script = BuildMarkupInjectionScript(markup_tags);
    base::Value::Dict js_params;
    js_params.Set("expression", inject_script);
    js_params.Set("returnByValue", true);
    js_params.Set("disableBreaks", true);

    client->SendCommand(
        "Runtime.evaluate", js_params,
        base::BindOnce(
            [](base::WeakPtr<AbpController> ctrl, std::string tid,
               std::vector<std::string> tags, ResponseCallback cb,
               bool success, const std::string& result) {
              if (!ctrl) return;
              auto* wc = ctrl->FindWebContents(tid);
              if (!wc) {
                ctrl->SendError(404, "Tab not found", std::move(cb));
                return;
              }
              auto* view = wc->GetRenderWidgetHostView();
              if (!view) {
                ctrl->SendError(500, "No render view", std::move(cb));
                return;
              }
              auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
                  view->GetRenderWidgetHost());

              rwhi->ForceRedrawWithCallback(base::BindOnce(
                  [](base::WeakPtr<AbpController> ctrl, std::string tid,
                     std::vector<std::string> tags, ResponseCallback cb) {
                    if (!ctrl) return;
                    // Wait 50ms for CoreAnimation then capture
                    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
                        FROM_HERE,
                        base::BindOnce(
                            [](base::WeakPtr<AbpController> ctrl,
                               std::string tid,
                               std::vector<std::string> tags,
                               ResponseCallback cb) {
                              if (!ctrl) return;
                              auto* wc = ctrl->FindWebContents(tid);
                              if (!wc) {
                                ctrl->SendError(404, "Tab not found",
                                                std::move(cb));
                                return;
                              }
                              // Cleanup markup (fire-and-forget)
                              auto* client = ctrl->GetOrCreateCdpClient(wc);
                              if (client) {
                                base::Value::Dict cleanup;
                                cleanup.Set("expression",
                                    AbpController::BuildMarkupCleanupScript(tags));
                                cleanup.Set("returnByValue", true);
                                cleanup.Set("disableBreaks", true);
                                client->SendCommand(
                                    "Runtime.evaluate", cleanup,
                                    base::BindOnce(
                                        [](bool, const std::string&) {}));
                              }
                              // Capture
                              gfx::NativeView native_view =
                                  wc->GetContentNativeView();
                              if (!native_view) {
                                std::move(cb).Run(500, "application/json",
                                    R"({"error":"No native view"})");
                                return;
                              }
                              auto* v = wc->GetRenderWidgetHostView();
                              gfx::Rect source_rect;
                              int vw = 0, vh = 0;
                              if (v) {
                                source_rect =
                                    gfx::Rect(v->GetViewBounds().size());
                                gfx::Size vps = v->GetVisibleViewportSize();
                                vw = vps.width();
                                vh = vps.height();
                              }
                              ui::GrabViewSnapshot(
                                  native_view, source_rect,
                                  base::BindOnce(
                                      [](ResponseCallback cb,
                                         int vp_w, int vp_h,
                                         gfx::Image snapshot) {
                                        if (snapshot.IsEmpty()) {
                                          std::move(cb).Run(
                                              500, "application/json",
                                              R"({"error":"Screenshot capture failed"})");
                                          return;
                                        }
                                        const SkBitmap& raw =
                                            *snapshot.ToSkBitmap();
                                        const SkBitmap bitmap =
                                            ScaleBitmapToViewport(raw, vp_w, vp_h);
                                        auto encoded =
                                            gfx::WebpCodec::Encode(bitmap, 80);
                                        if (!encoded || encoded->empty()) {
                                          std::move(cb).Run(
                                              500, "application/json",
                                              R"({"error":"Failed to encode screenshot"})");
                                          return;
                                        }
                                        std::string binary(encoded->begin(),
                                                           encoded->end());
                                        std::move(cb).Run(200, "image/webp",
                                                          std::move(binary));
                                      },
                                      std::move(cb), vw, vh));
                            },
                            ctrl, tid, tags, std::move(cb)),
                        kCoreAnimationDelay);
                  },
                  ctrl->weak_factory_.GetWeakPtr(), tid, tags,
                  std::move(cb)));
            },
            weak_factory_.GetWeakPtr(), tab_id, markup_tags,
            std::move(wrapped_cb)));
    return;
  }

  // No markup — direct capture path
  EnsureCompositorActive(
      tab_id,
      base::BindOnce(
          [](base::WeakPtr<AbpController> ctrl, std::string tid,
             ResponseCallback cb) {
            if (!ctrl) return;
            auto* wc = ctrl->FindWebContents(tid);
            if (!wc) {
              ctrl->SendError(404, "Tab not found", std::move(cb));
              return;
            }
            gfx::NativeView native_view = wc->GetContentNativeView();
            if (!native_view) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"No native view"})");
              return;
            }
            auto* view = wc->GetRenderWidgetHostView();
            gfx::Rect source_rect;
            int vw = 0, vh = 0;
            if (view) {
              source_rect = gfx::Rect(view->GetViewBounds().size());
              gfx::Size vps = view->GetVisibleViewportSize();
              vw = vps.width();
              vh = vps.height();
            }
            ui::GrabViewSnapshot(
                native_view, source_rect,
                base::BindOnce(
                    [](ResponseCallback cb, int vp_w, int vp_h,
                       gfx::Image snapshot) {
                      if (snapshot.IsEmpty()) {
                        std::move(cb).Run(
                            500, "application/json",
                            R"({"error":"Screenshot capture failed"})");
                        return;
                      }
                      const SkBitmap& raw = *snapshot.ToSkBitmap();
                      const SkBitmap bitmap =
                          ScaleBitmapToViewport(raw, vp_w, vp_h);
                      auto encoded = gfx::WebpCodec::Encode(bitmap, 80);
                      if (!encoded || encoded->empty()) {
                        std::move(cb).Run(
                            500, "application/json",
                            R"({"error":"Failed to encode screenshot"})");
                        return;
                      }
                      std::string binary(encoded->begin(), encoded->end());
                      std::move(cb).Run(200, "image/webp",
                                        std::move(binary));
                    },
                    std::move(cb), vw, vh));
          },
          weak_factory_.GetWeakPtr(), tab_id, std::move(wrapped_cb)));
}

// Browser shutdown endpoint

void AbpController::ShutdownBrowser(const base::Value::Dict& params,
                                    ResponseCallback callback) {
  // Get optional timeout
  int timeout_ms = params.FindInt("timeout_ms").value_or(5000);

  VLOG(1) << "ABP: Browser shutdown requested with timeout " << timeout_ms << "ms";

  // Send response before shutting down
  base::Value::Dict response;
  response.Set("success", true);
  response.Set("message", "Browser shutdown initiated");
  SendJson(200, base::Value(std::move(response)), std::move(callback));

  // Schedule shutdown after a brief delay to allow response to be sent
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce([]() {
        chrome::AttemptExit();
      }),
      base::Milliseconds(100));
}

// Download endpoints

void AbpController::SetDownloadObserver(AbpDownloadObserver* observer) {
  download_observer_ = observer;
}

void AbpController::SetEventObserver(AbpEventObserver* observer) {
  event_observer_ = observer;
}

void AbpController::SetTimingConfig(const AbpConfig::TimingConfig& timing) {
  timing_config_ = timing;
}

AbpActionContext::Options AbpController::GetDefaultActionOptions() const {
  AbpActionContext::Options options;
  options.min_wait_time = timing_config_.min_wait;
  options.request_tracking_timeout = timing_config_.tracking_timeout;
  options.post_tracking_settle_time = timing_config_.post_settle;
  return options;
}

void AbpController::SetSessionDir(const base::FilePath& session_dir) {
  session_dir_ = session_dir;
  network_db_ = std::make_unique<AbpNetworkDatabase>(session_dir_);
  curl_handler_ = std::make_unique<AbpCurlHandler>();
}

void AbpController::ListDownloads(const std::string& query,
                                  ResponseCallback callback) {
  if (!download_observer_) {
    SendError(503, "Download observer not available", std::move(callback));
    return;
  }

  // Parse query params: state=in_progress, limit=100
  std::string state_filter;
  int limit = 100;

  if (!query.empty()) {
    size_t state_pos = query.find("state=");
    if (state_pos != std::string::npos) {
      size_t start = state_pos + 6;
      size_t end = query.find('&', start);
      state_filter = query.substr(start, end == std::string::npos ? end : end - start);
    }

    size_t limit_pos = query.find("limit=");
    if (limit_pos != std::string::npos) {
      size_t start = limit_pos + 6;
      size_t end = query.find('&', start);
      std::string limit_str = query.substr(start, end == std::string::npos ? end : end - start);
      base::StringToInt(limit_str, &limit);
    }
  }

  auto downloads = download_observer_->GetDownloads(state_filter, limit);

  base::Value::List downloads_list;
  for (const auto& dl : downloads) {
    base::Value::Dict item;
    item.Set("id", dl.id);
    item.Set("url", dl.url);
    item.Set("filename", dl.filename);
    item.Set("path", dl.path);
    item.Set("state", dl.state);
    item.Set("bytes_received", static_cast<double>(dl.bytes_received));
    item.Set("total_bytes", static_cast<double>(dl.total_bytes));
    item.Set("mime_type", dl.mime_type);
    item.Set("start_time", static_cast<double>(dl.start_time_ms));
    if (dl.end_time_ms > 0) {
      item.Set("end_time", static_cast<double>(dl.end_time_ms));
    }
    downloads_list.Append(std::move(item));
  }

  base::Value::Dict response;
  response.Set("downloads", std::move(downloads_list));
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::GetDownloadStatus(const std::string& download_id,
                                      ResponseCallback callback) {
  if (!download_observer_) {
    SendError(503, "Download observer not available", std::move(callback));
    return;
  }

  auto download = download_observer_->GetDownload(download_id);
  if (!download) {
    SendError(404, "Download not found", std::move(callback));
    return;
  }

  base::Value::Dict response;
  response.Set("id", download->id);
  response.Set("url", download->url);
  response.Set("filename", download->filename);
  response.Set("path", download->path);
  response.Set("state", download->state);
  response.Set("bytes_received", static_cast<double>(download->bytes_received));
  response.Set("total_bytes", static_cast<double>(download->total_bytes));
  response.Set("mime_type", download->mime_type);
  response.Set("start_time", static_cast<double>(download->start_time_ms));
  if (download->end_time_ms > 0) {
    response.Set("end_time", static_cast<double>(download->end_time_ms));
  }

  // Calculate percent complete
  if (download->total_bytes > 0) {
    double percent = (static_cast<double>(download->bytes_received) /
                      static_cast<double>(download->total_bytes)) * 100.0;
    response.Set("percent_complete", percent);
  }

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::CancelDownload(const std::string& download_id,
                                   ResponseCallback callback) {
  if (!download_observer_) {
    SendError(503, "Download observer not available", std::move(callback));
    return;
  }

  if (download_observer_->CancelDownload(download_id)) {
    base::Value::Dict response;
    response.Set("success", true);
    response.Set("message", "Download cancelled");
    SendJson(200, base::Value(std::move(response)), std::move(callback));
  } else {
    SendError(400, "Failed to cancel download (may be already completed)",
              std::move(callback));
  }
}

void AbpController::HandleDownloadContent(const std::string& download_id,
                                          int64_t max_size,
                                          ResponseCallback callback) {
  if (!download_observer_) {
    SendError(503, "Download observer not available", std::move(callback));
    return;
  }

  auto download = download_observer_->GetDownload(download_id);
  if (!download) {
    SendError(404, "Download not found", std::move(callback));
    return;
  }

  if (download->state != "completed") {
    SendError(400, "Download not completed (state: " + download->state + ")",
              std::move(callback));
    return;
  }

  if (download->path.empty()) {
    SendError(404, "Download file path not available", std::move(callback));
    return;
  }

  base::FilePath file_path = base::FilePath::FromUTF8Unsafe(download->path);
  std::string mime = download->mime_type;
  std::string filename = download->filename;
  std::string dl_id = download->id;

  // Read file on thread pool (file I/O must not block UI thread).
  // Return pair: {error_code (0=ok, 413=too large, 500=read fail), data}.
  // All file I/O happens in the task lambda; the reply callback only touches
  // the result on the UI thread.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](base::FilePath path, int64_t max_size)
              -> std::pair<int, std::vector<uint8_t>> {
            std::optional<int64_t> file_size = base::GetFileSize(path);
            if (!file_size.has_value())
              return {500, {}};
            if (*file_size > max_size)
              return {413, {}};
            std::optional<std::vector<uint8_t>> data =
                base::ReadFileToBytes(path);
            if (!data.has_value())
              return {500, {}};
            return {0, std::move(*data)};
          },
          file_path, max_size),
      base::BindOnce(
          [](base::WeakPtr<AbpController> self, ResponseCallback cb,
             std::string mime, std::string filename, std::string dl_id,
             int64_t max_size,
             std::pair<int, std::vector<uint8_t>> result) {
            if (!self) {
              std::move(cb).Run(500, "application/json",
                                R"({"error":"Controller destroyed"})");
              return;
            }

            if (result.first == 413) {
              self->SendError(
                  413,
                  "File too large (max " +
                      base::NumberToString(max_size) + " bytes)",
                  std::move(cb));
              return;
            }

            if (result.first != 0) {
              self->SendError(500, "Failed to read download file",
                              std::move(cb));
              return;
            }

            // Return raw binary with Content-Type
            std::string content_type =
                mime.empty() ? "application/octet-stream" : mime;
            std::string body(result.second.begin(), result.second.end());
            std::move(cb).Run(200, content_type, std::move(body));
          },
          weak_factory_.GetWeakPtr(), std::move(callback), std::move(mime),
          std::move(filename), std::move(dl_id), max_size));
}

void AbpController::OnPermissionRequested(const std::string& perm_id,
                                          const std::string& tab_id,
                                          const std::string& permission_type,
                                          const std::string& origin) {
  PendingPermissionRequest req;
  req.id = perm_id;
  req.tab_id = tab_id;
  req.permission_type = permission_type;
  req.origin = origin;
  req.requested_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  pending_permissions_[perm_id] = std::move(req);
}

void AbpController::OnPermissionDismissed(const std::string& perm_id,
                                          const std::string& tab_id) {
  pending_permissions_.erase(perm_id);
}

void AbpController::ListPendingPermissions(ResponseCallback callback) {
  base::Value::List list;
  for (const auto& [id, req] : pending_permissions_) {
    base::Value::Dict d;
    d.Set("id", req.id);
    d.Set("tab_id", req.tab_id);
    d.Set("permission_type", req.permission_type);
    d.Set("origin", req.origin);
    d.Set("requested_at", static_cast<double>(req.requested_at_ms));
    list.Append(std::move(d));
  }
  base::Value::Dict response;
  response.Set("permissions", std::move(list));
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::GrantPermission(const std::string& perm_id,
                                    const base::Value::Dict& params,
                                    ResponseCallback callback) {
  auto it = pending_permissions_.find(perm_id);
  if (it == pending_permissions_.end()) {
    SendError(404, "No pending permission with id: " + perm_id,
              std::move(callback));
    return;
  }

  // Require permission_type and validate it matches the pending request.
  const std::string* req_type = params.FindString("permission_type");
  if (!req_type) {
    SendError(400, "Missing required 'permission_type'", std::move(callback));
    return;
  }
  if (*req_type != it->second.permission_type) {
    SendError(400,
              "permission_type mismatch: expected '" +
                  it->second.permission_type + "', got '" + *req_type + "'",
              std::move(callback));
    return;
  }

  std::string tab_id = it->second.tab_id;
  std::string permission_type = it->second.permission_type;

  // For geolocation grants, set mock coordinates from the request body.
  if (permission_type == "geolocation") {
    auto lat = params.FindDouble("latitude");
    auto lng = params.FindDouble("longitude");
    if (!lat || !lng) {
      SendError(400,
                "Granting geolocation requires 'latitude' and 'longitude'",
                std::move(callback));
      return;
    }
    double accuracy = params.FindDouble("accuracy").value_or(100.0);
    AbpLocationProvider::SetStoredPosition(*lat, *lng, accuracy);
  }

  // Remove from pending before action
  pending_permissions_.erase(it);

  AbpActionContext::RunWithOptions(
      this, tab_id, "permission_grant", params, GetDefaultActionOptions(),
      base::BindOnce(
          [](std::string perm_type, AbpActionContext* ctx) {
            auto* controller = ctx->controller();
            if (perm_type == "geolocation") {
              // Transition system permission to kAllowed so the provider can
              // start delivering coordinates after the web permission is
              // granted.
#if BUILDFLAG(OS_LEVEL_GEOLOCATION_PERMISSION_SUPPORTED)
              AbpSystemGeolocationSource::GrantSystemPermission();
#endif
            }
            if (!controller->permission_observer()->GrantPermission(
                    ctx->tab_id())) {
              ctx->OnActionError("PERMISSION_ERROR",
                                 "Failed to grant " + perm_type +
                                     " permission");
              return;
            }
            base::Value::Dict res;
            res.Set("status", "granted");
            res.Set("permission_type", perm_type);
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          std::move(permission_type)),
      std::move(callback));
}

void AbpController::DenyPermission(const std::string& perm_id,
                                   const base::Value::Dict& params,
                                   ResponseCallback callback) {
  auto it = pending_permissions_.find(perm_id);
  if (it == pending_permissions_.end()) {
    SendError(404, "No pending permission with id: " + perm_id,
              std::move(callback));
    return;
  }

  // Require permission_type and validate it matches the pending request.
  const std::string* req_type = params.FindString("permission_type");
  if (!req_type) {
    SendError(400, "Missing required 'permission_type'", std::move(callback));
    return;
  }
  if (*req_type != it->second.permission_type) {
    SendError(400,
              "permission_type mismatch: expected '" +
                  it->second.permission_type + "', got '" + *req_type + "'",
              std::move(callback));
    return;
  }

  std::string tab_id = it->second.tab_id;
  std::string permission_type = it->second.permission_type;

  pending_permissions_.erase(it);

  AbpActionContext::RunWithOptions(
      this, tab_id, "permission_deny", params, GetDefaultActionOptions(),
      base::BindOnce(
          [](std::string perm_type, AbpActionContext* ctx) {
            auto* controller = ctx->controller();
            if (!controller->permission_observer()->DenyPermission(
                    ctx->tab_id())) {
              ctx->OnActionError("PERMISSION_ERROR",
                                 "Failed to deny " + perm_type +
                                     " permission");
              return;
            }
            base::Value::Dict res;
            res.Set("status", "denied");
            res.Set("permission_type", perm_type);
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          std::move(permission_type)),
      std::move(callback));
}

// ---------------------------------------------------------------------------
// Network capture REST endpoints
// ---------------------------------------------------------------------------

// Helper: extract a single query param value from a query string.
// Returns empty string if the param is not found.
static std::string GetQueryParam(const std::string& query,
                                  const std::string& name) {
  std::string search = name + "=";
  size_t pos = query.find(search);
  if (pos == std::string::npos) return {};
  size_t start = pos + search.size();
  size_t end = query.find('&', start);
  return query.substr(start, end == std::string::npos ? end : end - start);
}

void AbpController::HandleNetworkQuery(const std::string& query_string,
                                       ResponseCallback callback) {
  // Parse common filter params from query string.
  std::string tag = GetQueryParam(query_string, "tag");
  std::string tab_id_param = GetQueryParam(query_string, "tab_id");
  std::string action_id = GetQueryParam(query_string, "action_id");
  std::string url_regex = GetQueryParam(query_string, "url");
  std::string hostname_regex = GetQueryParam(query_string, "hostname");
  std::string path_regex = GetQueryParam(query_string, "path");
  std::string query_regex = GetQueryParam(query_string, "query");
  std::string method_regex = GetQueryParam(query_string, "method");
  std::string status_regex = GetQueryParam(query_string, "status");
  std::string type = GetQueryParam(query_string, "type");
  std::string include_body_str = GetQueryParam(query_string, "include_body");
  bool include_body = (include_body_str == "true" || include_body_str == "1");
  std::string max_body_size_str =
      GetQueryParam(query_string, "max_body_size");
  int max_body_size = 0;
  if (!max_body_size_str.empty()) {
    base::StringToInt(max_body_size_str, &max_body_size);
  }

  // Query the in-memory buffer first.
  base::Value::List buffer_results;
  {
    abp::NetworkQueryFilter buf_filter;
    buf_filter.tag = tag;
    buf_filter.tab_id = tab_id_param;
    buf_filter.action_id = action_id;
    buf_filter.url_regex = url_regex;
    buf_filter.hostname_regex = hostname_regex;
    buf_filter.path_regex = path_regex;
    buf_filter.query_regex = query_regex;
    buf_filter.method_regex = method_regex;
    buf_filter.status_regex = status_regex;
    buf_filter.type = type;
    buf_filter.include_body = include_body;
    buf_filter.max_body_size = max_body_size;

    if (!tab_id_param.empty()) {
      // Query a specific tab's buffer.
      auto it = tab_states_.find(tab_id_param);
      if (it != tab_states_.end() && it->second.network_capture) {
        buffer_results =
            it->second.network_capture->QueryBuffer(buf_filter, tab_id_param);
      }
    } else {
      // Query the active tab's buffer.
      std::string active_id = GetActiveTabId();
      if (!active_id.empty()) {
        auto it = tab_states_.find(active_id);
        if (it != tab_states_.end() && it->second.network_capture) {
          buffer_results =
              it->second.network_capture->QueryBuffer(buf_filter, active_id);
        }
      }
    }
  }

  // Build dedup set from buffer results: (tab_id, request_id) pairs.
  std::set<std::pair<std::string, std::string>> seen_keys;
  for (const auto& val : buffer_results) {
    if (val.is_dict()) {
      const std::string* tid = val.GetDict().FindString("tab_id");
      const std::string* rid = val.GetDict().FindString("request_id");
      if (tid && rid) {
        seen_keys.emplace(*tid, *rid);
      }
    }
  }

  // If no database, return buffer results only.
  if (!network_db_) {
    base::Value::Dict response;
    response.Set("requests", std::move(buffer_results));
    std::string json;
    base::JSONWriter::Write(response, &json);
    std::move(callback).Run(200, "application/json", std::move(json));
    return;
  }

  // Build the database filter.
  AbpNetworkDatabase::QueryFilter db_filter;
  db_filter.tag = tag;
  db_filter.tab_id = tab_id_param;
  db_filter.action_id = action_id;
  db_filter.url_regex = url_regex;
  db_filter.hostname_regex = hostname_regex;
  db_filter.path_regex = path_regex;
  db_filter.query_regex = query_regex;
  db_filter.method_regex = method_regex;
  db_filter.status_regex = status_regex;
  db_filter.type = type;
  db_filter.include_body = include_body;
  db_filter.max_body_size = max_body_size;

  network_db_->QueryRequests(
      db_filter,
      base::BindOnce(
          [](ResponseCallback cb, base::Value::List buffer_results,
             std::set<std::pair<std::string, std::string>> seen_keys,
             base::Value::List db_results) {
            // Merge: buffer results first, then DB results not in buffer.
            base::Value::List merged;
            for (auto& val : buffer_results) {
              merged.Append(std::move(val));
            }
            for (auto& val : db_results) {
              if (val.is_dict()) {
                const std::string* tid = val.GetDict().FindString("tab_id");
                const std::string* rid =
                    val.GetDict().FindString("request_id");
                if (tid && rid &&
                    seen_keys.count(std::make_pair(*tid, *rid))) {
                  continue;  // Skip duplicate.
                }
              }
              merged.Append(std::move(val));
            }
            base::Value::Dict response;
            response.Set("requests", std::move(merged));
            std::string json;
            base::JSONWriter::Write(response, &json);
            std::move(cb).Run(200, "application/json", std::move(json));
          },
          std::move(callback), std::move(buffer_results),
          std::move(seen_keys)));
}

void AbpController::HandleNetworkSave(const std::string& body,
                                      ResponseCallback callback) {
  if (!network_db_) {
    SendError(503, "Network database not available", std::move(callback));
    return;
  }

  auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    SendError(400, "Invalid JSON body", std::move(callback));
    return;
  }
  const base::Value::Dict& params = parsed->GetDict();

  const std::string* tag = params.FindString("tag");
  if (!tag || tag->empty()) {
    SendError(400, "Missing required field: tag", std::move(callback));
    return;
  }
  const std::string* tab_id = params.FindString("tab_id");
  std::string resolved_tab_id = (tab_id && !tab_id->empty()) ? *tab_id : GetActiveTabId();
  if (resolved_tab_id.empty()) {
    SendError(400, "No active tab", std::move(callback));
    return;
  }

  // Find the tab's network capture buffer.
  auto it = tab_states_.find(resolved_tab_id);
  if (it == tab_states_.end() || !it->second.network_capture) {
    SendError(404, "Tab not found or no capture buffer", std::move(callback));
    return;
  }

  const std::vector<abp::CapturedRequest>& requests =
      it->second.network_capture->GetRequests();

  int saved_count = static_cast<int>(requests.size());
  network_db_->SaveRequests(*tag, resolved_tab_id, requests,
                            base::BindOnce(
                                [](ResponseCallback cb, int count,
                                   const std::string& t) {
                                  base::Value::Dict response;
                                  response.Set("saved", count);
                                  response.Set("tag", t);
                                  std::string json;
                                  base::JSONWriter::Write(response, &json);
                                  std::move(cb).Run(200, "application/json",
                                                    std::move(json));
                                },
                                std::move(callback), saved_count, *tag));
}

void AbpController::HandleNetworkClear(const std::string& query_string,
                                       ResponseCallback callback) {
  if (!network_db_) {
    SendError(503, "Network database not available", std::move(callback));
    return;
  }

  std::string tag = GetQueryParam(query_string, "tag");

  network_db_->ClearRequests(
      tag, base::BindOnce(
               [](ResponseCallback cb) {
                 base::Value::Dict response;
                 response.Set("status", "cleared");
                 std::string json;
                 base::JSONWriter::Write(response, &json);
                 std::move(cb).Run(200, "application/json", std::move(json));
               },
               std::move(callback)));
}

void AbpController::HandleConsoleQuery(const std::string& query_string,
                                       ResponseCallback callback) {
  if (!console_capture_) {
    SendError(503, "Console capture not initialized", std::move(callback));
    return;
  }

  std::string tab_id = GetQueryParam(query_string, "tab_id");
  std::string level = GetQueryParam(query_string, "level");
  std::string pattern = GetQueryParam(query_string, "pattern");
  std::string limit_str = GetQueryParam(query_string, "limit");
  std::string after_id_str = GetQueryParam(query_string, "after_id");

  int limit = 100;
  if (!limit_str.empty()) {
    base::StringToInt(limit_str, &limit);
  }
  int64_t after_id = 0;
  if (!after_id_str.empty()) {
    base::StringToInt64(after_id_str, &after_id);
  }

  // Validate regex before querying.
  if (!pattern.empty()) {
    RE2 test_regex(pattern);
    if (!test_regex.ok()) {
      SendError(400, "Invalid regex pattern: " + test_regex.error(),
                std::move(callback));
      return;
    }
  }

  auto entries = console_capture_->Query(tab_id, level, pattern, limit,
                                         after_id);

  base::Value::List entries_list;
  for (const auto& entry : entries) {
    entries_list.Append(entry.ToDict());
  }

  base::Value::Dict response;
  response.Set("entries", std::move(entries_list));
  response.Set("total_buffered",
               static_cast<int>(console_capture_->Size()));
  response.Set("oldest_id",
               static_cast<double>(console_capture_->OldestId()));

  std::string json;
  base::JSONWriter::Write(response, &json);
  std::move(callback).Run(200, "application/json", std::move(json));
}

void AbpController::HandleConsoleClear(const std::string& query_string,
                                       ResponseCallback callback) {
  if (!console_capture_) {
    SendError(503, "Console capture not initialized", std::move(callback));
    return;
  }

  std::string tab_id = GetQueryParam(query_string, "tab_id");
  size_t cleared = console_capture_->Clear(tab_id);

  base::Value::Dict response;
  response.Set("cleared", static_cast<int>(cleared));

  std::string json;
  base::JSONWriter::Write(response, &json);
  std::move(callback).Run(200, "application/json", std::move(json));
}

void AbpController::HandleCurl(const std::string& tab_id,
                               const std::string& body,
                               ResponseCallback callback) {
  if (!curl_handler_) {
    SendError(503, "Curl handler not available", std::move(callback));
    return;
  }

  auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    SendError(400, "Invalid JSON body", std::move(callback));
    return;
  }
  const base::Value::Dict& params = parsed->GetDict();

  const std::string* url = params.FindString("url");
  if (!url || url->empty()) {
    SendError(400, "Missing required field: url", std::move(callback));
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  AbpCurlHandler::Request req;
  req.url = *url;

  const std::string* method = params.FindString("method");
  if (method) {
    req.method = *method;
  }

  const std::string* req_body = params.FindString("body");
  if (req_body) {
    req.body = *req_body;
  }

  const base::Value::Dict* headers = params.FindDict("headers");
  if (headers) {
    for (auto it_hdr = headers->begin(); it_hdr != headers->end(); ++it_hdr) {
      if (it_hdr->second.is_string()) {
        req.headers[it_hdr->first] = it_hdr->second.GetString();
      }
    }
  }

  // Auto-populate origin and referer from the tab's current URL.
  GURL current_url = wc->GetLastCommittedURL();
  if (current_url.is_valid()) {
    req.origin = current_url.DeprecatedGetOriginAsURL().spec();
    req.referer = current_url.spec();
  }

  // Get the StoragePartition for this tab (for session/cookie sharing).
  content::StoragePartition* storage_partition =
      wc->GetBrowserContext()->GetDefaultStoragePartition();

  // Parse optional save config.
  const std::string* save_tag = params.FindString("save_tag");

  curl_handler_->Execute(
      req, storage_partition,
      base::BindOnce(
          [](ResponseCallback cb,
             base::WeakPtr<AbpController> controller,
             std::string stab_id, std::string tag,
             AbpCurlHandler::Response response) {
            // Optionally save to network_db_.
            if (!tag.empty() && controller && controller->network_db_) {
              abp::CapturedRequest captured;
              captured.url = response.final_url.empty()
                                 ? std::string()
                                 : response.final_url;
              captured.method = "GET";
              captured.status_code = response.status_code;
              captured.response_body = response.body;
              captured.response_body_is_base64 = response.body_is_base64;
              for (const auto& [k, v] : response.headers) {
                captured.response_headers.Set(k, v);
              }
              captured.completed = true;
              std::vector<abp::CapturedRequest> reqs;
              reqs.push_back(std::move(captured));
              controller->network_db_->SaveRequests(tag, stab_id, reqs,
                                                    base::DoNothing());
            }

            base::Value::Dict result;
            result.Set("status_code", response.status_code);
            result.Set("body", response.body);
            result.Set("body_is_base64", response.body_is_base64);
            result.Set("final_url", response.final_url);
            result.Set("redirected", response.redirected);
            base::Value::Dict resp_headers;
            for (const auto& [k, v] : response.headers) {
              resp_headers.Set(k, v);
            }
            result.Set("headers", std::move(resp_headers));
            std::string json;
            base::JSONWriter::Write(result, &json);
            std::move(cb).Run(200, "application/json", std::move(json));
          },
          std::move(callback), weak_factory_.GetWeakPtr(), tab_id,
          save_tag ? *save_tag : std::string()));
}

}  // namespace abp
