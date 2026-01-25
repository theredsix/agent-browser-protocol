#include "chrome/browser/abp/abp_controller.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include "base/base64.h"
#include "chrome/browser/abp/abp_action_context.h"
#include "base/command_line.h"
#include "chrome/browser/abp/abp_switches.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_download_observer.h"
#include "chrome/browser/abp/abp_event_collector.h"
#include "chrome/browser/abp/abp_history_controller.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "ui/base/cursor/mojom/cursor_type.mojom.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/codec/webp_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace abp {

// ActionContext implementation
ActionContext::ActionContext() = default;
ActionContext::~ActionContext() = default;
ActionContext::ActionContext(ActionContext&&) = default;
ActionContext& ActionContext::operator=(ActionContext&&) = default;

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

// PendingDialog implementation
AbpController::PendingDialog::PendingDialog() = default;
AbpController::PendingDialog::~PendingDialog() = default;
AbpController::PendingDialog::PendingDialog(const PendingDialog&) = default;
AbpController::PendingDialog& AbpController::PendingDialog::operator=(
    const PendingDialog&) = default;

namespace {

// Key code mapping table for CDP Input.dispatchKeyEvent
// See: https://chromedevtools.github.io/devtools-protocol/tot/Input/#method-dispatchKeyEvent
struct KeyMapping {
  const char* name;
  const char* key;
  const char* code;
  int vk;
  bool is_modifier;
  int modifier_flag;  // 1=Alt, 2=Ctrl, 4=Meta, 8=Shift
};

// clang-format off
constexpr KeyMapping kKeyMappings[] = {
    // Modifiers
    {"Alt", "Alt", "AltLeft", 18, true, 1},
    {"AltLeft", "Alt", "AltLeft", 18, true, 1},
    {"AltRight", "Alt", "AltRight", 18, true, 1},
    {"Control", "Control", "ControlLeft", 17, true, 2},
    {"ControlLeft", "Control", "ControlLeft", 17, true, 2},
    {"ControlRight", "Control", "ControlRight", 17, true, 2},
    {"Meta", "Meta", "MetaLeft", 91, true, 4},
    {"MetaLeft", "Meta", "MetaLeft", 91, true, 4},
    {"MetaRight", "Meta", "MetaRight", 92, true, 4},
    {"Shift", "Shift", "ShiftLeft", 16, true, 8},
    {"ShiftLeft", "Shift", "ShiftLeft", 16, true, 8},
    {"ShiftRight", "Shift", "ShiftRight", 16, true, 8},

    // Special keys
    {"Enter", "Enter", "Enter", 13, false, 0},
    {"Tab", "Tab", "Tab", 9, false, 0},
    {"Escape", "Escape", "Escape", 27, false, 0},
    {"Backspace", "Backspace", "Backspace", 8, false, 0},
    {"Delete", "Delete", "Delete", 46, false, 0},
    {"Insert", "Insert", "Insert", 45, false, 0},
    {"Home", "Home", "Home", 36, false, 0},
    {"End", "End", "End", 35, false, 0},
    {"PageUp", "PageUp", "PageUp", 33, false, 0},
    {"PageDown", "PageDown", "PageDown", 34, false, 0},
    {"Space", " ", "Space", 32, false, 0},
    {" ", " ", "Space", 32, false, 0},

    // Arrow keys
    {"ArrowUp", "ArrowUp", "ArrowUp", 38, false, 0},
    {"ArrowDown", "ArrowDown", "ArrowDown", 40, false, 0},
    {"ArrowLeft", "ArrowLeft", "ArrowLeft", 37, false, 0},
    {"ArrowRight", "ArrowRight", "ArrowRight", 39, false, 0},

    // Function keys
    {"F1", "F1", "F1", 112, false, 0},
    {"F2", "F2", "F2", 113, false, 0},
    {"F3", "F3", "F3", 114, false, 0},
    {"F4", "F4", "F4", 115, false, 0},
    {"F5", "F5", "F5", 116, false, 0},
    {"F6", "F6", "F6", 117, false, 0},
    {"F7", "F7", "F7", 118, false, 0},
    {"F8", "F8", "F8", 119, false, 0},
    {"F9", "F9", "F9", 120, false, 0},
    {"F10", "F10", "F10", 121, false, 0},
    {"F11", "F11", "F11", 122, false, 0},
    {"F12", "F12", "F12", 123, false, 0},

    // Number row (digits and symbols need special handling)
    {"0", "0", "Digit0", 48, false, 0},
    {"1", "1", "Digit1", 49, false, 0},
    {"2", "2", "Digit2", 50, false, 0},
    {"3", "3", "Digit3", 51, false, 0},
    {"4", "4", "Digit4", 52, false, 0},
    {"5", "5", "Digit5", 53, false, 0},
    {"6", "6", "Digit6", 54, false, 0},
    {"7", "7", "Digit7", 55, false, 0},
    {"8", "8", "Digit8", 56, false, 0},
    {"9", "9", "Digit9", 57, false, 0},
};
// clang-format on

}  // namespace

// Key info helper - defined outside anonymous namespace so it's accessible
KeyInfo GetKeyInfo(const std::string& key_name) {
  // First check the mapping table
  for (const auto& mapping : kKeyMappings) {
    if (key_name == mapping.name) {
      return {mapping.key, mapping.code, mapping.vk, mapping.vk,
              mapping.is_modifier, mapping.modifier_flag};
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
    if (mod == "Alt" || mod == "AltLeft" || mod == "AltRight") {
      flags |= 1;
    } else if (mod == "Control" || mod == "ControlLeft" || mod == "ControlRight") {
      flags |= 2;
    } else if (mod == "Meta" || mod == "MetaLeft" || mod == "MetaRight") {
      flags |= 4;
    } else if (mod == "Shift" || mod == "ShiftLeft" || mod == "ShiftRight") {
      flags |= 8;
    }
  }
  return flags;
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

// Process screenshot bitmap and save to file (runs on background thread)
// Note: Cursor rendering is handled by the virtual cursor overlay layer
// (InspectorCursorDrawer) which is captured automatically via CopyFromSurface.
void ProcessAndSaveScreenshot(
    const base::FilePath& screenshot_path,
    base::OnceCallback<void(std::string path)> callback,
    SkBitmap bitmap) {
  // This runs on a background thread

  // Use the bitmap directly - it was already copied when passed here
  SkBitmap output_bitmap = std::move(bitmap);

  // Encode as WebP
  std::optional<std::vector<uint8_t>> encoded =
      gfx::WebpCodec::Encode(output_bitmap, 80);

  if (!encoded || encoded->empty()) {
    LOG(WARNING) << "ABP: Failed to encode screenshot as WebP";
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), std::string()));
    return;
  }

  // Create parent directory if needed
  base::FilePath dir = screenshot_path.DirName();
  if (!base::DirectoryExists(dir)) {
    base::CreateDirectory(dir);
  }

  // Write to file
  bool success = base::WriteFile(screenshot_path, *encoded);

  if (success) {
    LOG(INFO) << "ABP: Saved screenshot to " << screenshot_path.value();
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), screenshot_path.value()));
  } else {
    LOG(WARNING) << "ABP: Failed to write screenshot to "
                 << screenshot_path.value();
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), std::string()));
  }
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
    : event_collector_(std::make_unique<AbpEventCollector>(this)) {}

AbpController::~AbpController() = default;

void AbpController::SetHistoryController(
    AbpHistoryController* history_controller) {
  history_controller_ = history_controller;
}

void AbpController::CenterMouseInActiveTab() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Get the first browser with an active tab
  Browser* browser = nullptr;
  const BrowserList* browser_list = BrowserList::GetInstance();
  for (auto it = browser_list->begin(); it != browser_list->end(); ++it) {
    if ((*it)->tab_strip_model()->count() > 0) {
      browser = *it;
      break;
    }
  }

  if (!browser) {
    LOG(WARNING) << "ABP: No browser available for mouse centering";
    return;
  }

  content::WebContents* wc =
      browser->tab_strip_model()->GetActiveWebContents();
  if (!wc) {
    LOG(WARNING) << "ABP: No active WebContents for mouse centering";
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    LOG(WARNING) << "ABP: No RenderWidgetHostView for mouse centering";
    return;
  }

  // Get viewport size
  gfx::Size viewport_size = rwhv->GetVisibleViewportSize();
  double center_x = viewport_size.width() / 2.0;
  double center_y = viewport_size.height() / 2.0;

  LOG(INFO) << "ABP: Centering mouse at (" << center_x << ", " << center_y
            << ") in viewport " << viewport_size.width() << "x"
            << viewport_size.height();

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    LOG(WARNING) << "ABP: Failed to create CDP client for mouse centering";
    return;
  }

  // Update virtual cursor state so screenshots know the cursor position
  scoped_refptr<content::DevToolsAgentHost> host =
      content::DevToolsAgentHost::GetOrCreateForTab(wc);
  if (host) {
    VirtualCursorState& state = virtual_cursor_states_[host->GetId()];
    state.active = true;
    state.x = center_x;
    state.y = center_y;
  }

  // First, set the virtual cursor position so it appears in screenshots
  base::Value::Dict cursor_config;
  cursor_config.Set("x", center_x);
  cursor_config.Set("y", center_y);
  cursor_config.Set("visible", true);

  base::Value::Dict cursor_params;
  cursor_params.Set("cursorConfig", std::move(cursor_config));

  client->SendCommand(
      "Overlay.setVirtualCursor", cursor_params,
      base::BindOnce(
          [](base::WeakPtr<AbpController> controller, AbpCdpClient* client,
             double x, double y, bool success, const std::string& result) {
            if (!controller || !client) {
              return;
            }

            // Also send mouseMoved event for page interaction
            base::Value::Dict cdp_params;
            cdp_params.Set("type", "mouseMoved");
            cdp_params.Set("x", x);
            cdp_params.Set("y", y);

            client->SendCommand(
                "Input.dispatchMouseEvent", cdp_params,
                base::BindOnce([](bool success, const std::string& result) {
                  if (success) {
                    LOG(INFO) << "ABP: Mouse centered successfully";
                  } else {
                    LOG(WARNING) << "ABP: Failed to center mouse: " << result;
                  }
                }));
          },
          weak_factory_.GetWeakPtr(), client, center_x, center_y));
}

bool AbpController::IsBrowserReady() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  const BrowserList* browser_list = BrowserList::GetInstance();
  for (auto it = browser_list->begin(); it != browser_list->end(); ++it) {
    Browser* browser = *it;
    if (browser->tab_strip_model()->count() > 0) {
      content::WebContents* wc =
          browser->tab_strip_model()->GetActiveWebContents();
      if (wc && wc->GetRenderWidgetHostView()) {
        return true;
      }
    }
  }
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
        AbpCdpClient* client = GetOrCreateCdpClient(wc);
        if (client) {
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
  data.Set("components", std::move(components));

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

void AbpController::CaptureScreenshotForHistory(
    const std::string& tab_id,
    int64_t timestamp,
    bool is_before,
    base::OnceCallback<void(std::string path)> callback) {
  if (!history_controller_ || !history_controller_->ScreenshotsEnabled()) {
    std::move(callback).Run("");
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run("");
    return;
  }

  base::FilePath screenshot_path =
      history_controller_->GetScreenshotPath(tab_id, timestamp, is_before);

  // Use direct C++ capture with CopyFromSurface
  // Note: Cursor is rendered by virtual cursor overlay and captured automatically
  CaptureScreenshotDirect(wc, screenshot_path, std::move(callback));
}

void AbpController::CaptureScreenshotDirect(
    content::WebContents* web_contents,
    const base::FilePath& screenshot_path,
    base::OnceCallback<void(std::string path)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!web_contents) {
    std::move(callback).Run("");
    return;
  }

  content::RenderWidgetHostView* rwhv =
      web_contents->GetRenderWidgetHostView();
  if (!rwhv) {
    LOG(WARNING) << "ABP: No RenderWidgetHostView for screenshot";
    std::move(callback).Run("");
    return;
  }

  // Capture the surface directly with 5 second timeout
  // Note: The virtual cursor is rendered by InspectorCursorDrawer in the
  // inspector overlay layer, which is included automatically in CopyFromSurface.
  rwhv->CopyFromSurface(
      gfx::Rect(),   // empty = full viewport
      gfx::Size(),   // empty = native resolution
      base::Seconds(5),  // timeout
      base::BindOnce(&AbpController::OnSurfaceCopied,
                     weak_factory_.GetWeakPtr(),
                     screenshot_path,
                     std::move(callback)));
}

void AbpController::OnSurfaceCopied(
    const base::FilePath& screenshot_path,
    base::OnceCallback<void(std::string path)> callback,
    const content::CopyFromSurfaceResult& result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!result.has_value()) {
    LOG(WARNING) << "ABP: CopyFromSurface failed: " << result.error();
    std::move(callback).Run("");
    return;
  }

  const SkBitmap& bitmap = result->bitmap;
  if (bitmap.empty()) {
    LOG(WARNING) << "ABP: CopyFromSurface returned empty bitmap";
    std::move(callback).Run("");
    return;
  }

  // Make a deep copy to pass to the background thread
  SkBitmap bitmap_copy;
  bitmap_copy.allocPixels(bitmap.info());
  bitmap.readPixels(bitmap_copy.info(), bitmap_copy.getPixels(),
                    bitmap_copy.rowBytes(), 0, 0);

  // Process and save on a background thread (using free function in anon namespace)
  // Note: Cursor is already in the bitmap from the virtual cursor overlay layer
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::TaskPriority::USER_VISIBLE, base::MayBlock()},
      base::BindOnce(&ProcessAndSaveScreenshot,
                     screenshot_path,
                     std::move(callback),
                     std::move(bitmap_copy)));
}

void AbpController::OnHistoryMarkupInjected(
    const std::string& tab_id,
    const base::FilePath& screenshot_path,
    base::OnceCallback<void(std::string path)> callback,
    bool success,
    const std::string& result) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run("");
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(callback).Run("");
    return;
  }

  // Take screenshot via CDP
  base::Value::Dict cdp_params;
  cdp_params.Set("format", "webp");
  cdp_params.Set("quality", 80);

  client->SendCommand(
      "Page.captureScreenshot", cdp_params,
      base::BindOnce(&AbpController::OnHistoryScreenshotCaptured,
                     weak_factory_.GetWeakPtr(), tab_id, screenshot_path,
                     std::move(callback)));
}

void AbpController::OnHistoryScreenshotCaptured(
    const std::string& tab_id,
    const base::FilePath& screenshot_path,
    base::OnceCallback<void(std::string path)> callback,
    bool success,
    const std::string& result) {
  // Clean up injected styles
  content::WebContents* wc = FindWebContents(tab_id);
  if (wc) {
    AbpCdpClient* client = GetOrCreateCdpClient(wc);
    if (client) {
      std::string cleanup_script = R"(
        (function() {
          const style = document.getElementById('abp-history-style');
          if (style) style.remove();
          const cursor = document.getElementById('abp-cursor-indicator');
          if (cursor) cursor.remove();
          return true;
        })()
      )";
      base::Value::Dict cleanup_params;
      cleanup_params.Set("expression", cleanup_script);
      client->SendCommand("Runtime.evaluate", cleanup_params,
                          base::DoNothing());
    }
  }

  if (!success) {
    std::move(callback).Run("");
    return;
  }

  auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    std::move(callback).Run("");
    return;
  }

  const std::string* data = parsed->GetDict().FindString("data");
  if (!data) {
    std::move(callback).Run("");
    return;
  }

  // Decode and write file
  std::optional<std::vector<uint8_t>> decoded = base::Base64Decode(*data);
  if (!decoded) {
    std::move(callback).Run("");
    return;
  }

  // Write to file on ThreadPool
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath p, std::vector<uint8_t> content) -> std::string {
            if (base::WriteFile(p, content)) {
              return p.value();
            }
            return "";
          },
          screenshot_path, std::move(*decoded)),
      std::move(callback));
}

void AbpController::RecordCompletedAction(
    const ActionContext& context,
    const base::Value* result,
    bool success,
    const std::string& error_code,
    const std::string& error_message,
    const std::string& screenshot_after_path) {
  if (!history_controller_) {
    return;
  }

  int64_t end_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  int64_t duration_ms = end_time - context.start_time;

  history_controller_->RecordAction(
      context.tab_id, context.action_type, context.params, result, success,
      error_code, error_message, context.start_time, duration_ms,
      context.screenshot_before_path, screenshot_after_path);
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

  // Parse path: /api/v1/tabs, /api/v1/tabs/{id}, /api/v1/tabs/{id}/action
  std::vector<std::string> segments = ParsePath(path);

  // Validate /api/v1 prefix
  if (segments.size() < 3 || segments[0] != "api" || segments[1] != "v1") {
    SendError(404, "Not found", std::move(callback));
    return;
  }

  const std::string& resource = segments[2];

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
          // Extract query string for markup option
          size_t query_pos = path.find('?');
          std::string query = (query_pos != std::string::npos)
                                  ? path.substr(query_pos + 1)
                                  : "";
          BinaryScreenshot(tab_id, query, std::move(callback));
        } else {
          // POST returns JSON with base64 data
          Screenshot(tab_id, params, std::move(callback));
        }
      } else if (action == "execute") {
        ExecuteScript(tab_id, params, std::move(callback));
      } else if (action == "click") {
        Click(tab_id, params, std::move(callback));
      } else if (action == "type") {
        Type(tab_id, params, std::move(callback));
      } else if (action == "move") {
        Move(tab_id, params, std::move(callback));
      } else if (action == "wait") {
        Wait(tab_id, params, std::move(callback));
      } else if (action == "scroll") {
        Scroll(tab_id, params, std::move(callback));
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
      history_controller_->RecordAction("", "create_tab", params, nullptr,
                                        false, "NO_BROWSER", "No active browser",
                                        start_time, 0, "", "");
    }
    SendError(500, "No active browser", std::move(callback));
    return;
  }

  const std::string* url = params.FindString("url");
  GURL gurl = url ? GURL(*url) : GURL("about:blank");

  NavigateParams nav_params(browser, gurl, ui::PAGE_TRANSITION_TYPED);
  nav_params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  ::Navigate(&nav_params);

  if (nav_params.navigated_or_inserted_contents) {
    content::WebContents* wc = nav_params.navigated_or_inserted_contents;
    auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);

    base::Value::Dict tab;
    tab.Set("id", host->GetId());
    tab.Set("url", wc->GetVisibleURL().spec());

    // Record successful action
    if (history_controller_) {
      int64_t duration_ms =
          base::Time::Now().InMillisecondsSinceUnixEpoch() - start_time;
      base::Value result_value(tab.Clone());
      history_controller_->RecordAction(host->GetId(), "create_tab", params,
                                        &result_value, true, "", "", start_time,
                                        duration_ms, "", "");
    }

    SendJson(201, base::Value(std::move(tab)), std::move(callback));
  } else {
    if (history_controller_) {
      history_controller_->RecordAction("", "create_tab", params, nullptr,
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

  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      if (host->GetId() == tab_id) {
        // Clean up all per-tab state
        cdp_clients_.erase(tab_id);
        execution_states_.erase(tab_id);
        virtual_cursor_states_.erase(tab_id);
        held_keys_state_.erase(tab_id);
        tab_strip->CloseWebContentsAt(i, TabCloseTypes::CLOSE_USER_GESTURE);

        base::Value::Dict result;
        if (history_controller_) {
          int64_t duration_ms =
              base::Time::Now().InMillisecondsSinceUnixEpoch() - start_time;
          base::Value result_value(result.Clone());
          history_controller_->RecordAction(tab_id, "close_tab", params,
                                            &result_value, true, "", "",
                                            start_time, duration_ms, "", "");
        }

        SendJson(200, base::Value(std::move(result)), std::move(callback));
        return;
      }
    }
  }

  if (history_controller_) {
    history_controller_->RecordAction(tab_id, "close_tab", params, nullptr,
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

  // Use AbpActionContext with skip_execution_control=true
  // Navigation is async and the page needs JS to run during loading.
  // If execution is paused, user should resume manually before navigate.
  AbpActionContext::Options options;
  options.skip_execution_control = true;

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

void AbpController::OnNavigateBeforeScreenshot(
    const std::string& tab_id,
    const std::string& url,
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_before_path) {
  if (context) {
    context->screenshot_before_path = screenshot_before_path;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "TAB_NOT_FOUND", "Tab not found", context->start_time, 0,
          context->screenshot_before_path, "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  // NOTE: For navigate, we do NOT integrate with execution control.
  // Navigation is async and the page needs to run JS to complete loading.
  // If execution is paused, user should resume manually before navigate.
  DispatchNavigateEvent(tab_id, url, std::move(context), std::move(callback));
}

void AbpController::DispatchNavigateEvent(
    const std::string& tab_id,
    const std::string& url,
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  GURL gurl(url);
  wc->GetController().LoadURL(gurl, content::Referrer(),
                              ui::PAGE_TRANSITION_TYPED, std::string());

  // Use centralized action completion with wait
  // This will wait for page load, network idle, etc. before taking screenshot
  base::Value::Dict result;
  result.Set("status", "navigated");
  result.Set("url", url);
  CompleteActionWithScreenshot(tab_id, std::move(context), std::move(result),
                               std::move(callback));
}

void AbpController::OnNavigateAfterScreenshot(
    const std::string& url,
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  // Legacy callback - kept for compatibility but no longer used
  base::Value::Dict result;
  result.Set("status", "navigated");
  result.Set("url", url);

  if (history_controller_ && context) {
    int64_t duration_ms =
        base::Time::Now().InMillisecondsSinceUnixEpoch() - context->start_time;
    base::Value result_value(result.Clone());
    history_controller_->RecordAction(
        context->tab_id, context->action_type, context->params, &result_value,
        true, "", "", context->start_time, duration_ms,
        context->screenshot_before_path, screenshot_after_path);
  }

  SendJson(200, base::Value(std::move(result)), std::move(callback));
}

void AbpController::Reload(const std::string& tab_id,
                           ResponseCallback callback) {
  base::Value::Dict params;  // Empty params for reload

  // Use AbpActionContext with skip_execution_control=true
  // Reload is similar to Navigate - needs JS to run
  AbpActionContext::Options options;
  options.skip_execution_control = true;

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

void AbpController::OnReloadBeforeScreenshot(
    const std::string& tab_id,
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_before_path) {
  if (context) {
    context->screenshot_before_path = screenshot_before_path;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "TAB_NOT_FOUND", "Tab not found", context->start_time, 0,
          context->screenshot_before_path, "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  wc->GetController().Reload(content::ReloadType::NORMAL, false);

  // Use centralized action completion with wait
  base::Value::Dict result;
  result.Set("status", "reloaded");
  CompleteActionWithScreenshot(tab_id, std::move(context), std::move(result),
                               std::move(callback));
}

void AbpController::OnReloadAfterScreenshot(
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  // Legacy callback - kept for compatibility but no longer used
  base::Value::Dict result;
  result.Set("status", "reloaded");

  if (history_controller_ && context) {
    int64_t duration_ms =
        base::Time::Now().InMillisecondsSinceUnixEpoch() - context->start_time;
    base::Value result_value(result.Clone());
    history_controller_->RecordAction(
        context->tab_id, context->action_type, context->params, &result_value,
        true, "", "", context->start_time, duration_ms,
        context->screenshot_before_path, screenshot_after_path);
  }

  SendJson(200, base::Value(std::move(result)), std::move(callback));
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

  // Use AbpActionContext with skip_execution_control=true
  AbpActionContext::Options options;
  options.skip_execution_control = true;

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

void AbpController::OnGoBackBeforeScreenshot(
    const std::string& tab_id,
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_before_path) {
  if (context) {
    context->screenshot_before_path = screenshot_before_path;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "TAB_NOT_FOUND", "Tab not found", context->start_time, 0,
          context->screenshot_before_path, "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  wc->GetController().GoBack();

  // Use centralized action completion with wait
  base::Value::Dict result;
  result.Set("status", "navigated_back");
  CompleteActionWithScreenshot(tab_id, std::move(context), std::move(result),
                               std::move(callback));
}

void AbpController::OnGoBackAfterScreenshot(
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  // Legacy callback - kept for compatibility but no longer used
  base::Value::Dict result;
  result.Set("status", "navigated_back");

  if (history_controller_ && context) {
    int64_t duration_ms =
        base::Time::Now().InMillisecondsSinceUnixEpoch() - context->start_time;
    base::Value result_value(result.Clone());
    history_controller_->RecordAction(
        context->tab_id, context->action_type, context->params, &result_value,
        true, "", "", context->start_time, duration_ms,
        context->screenshot_before_path, screenshot_after_path);
  }

  SendJson(200, base::Value(std::move(result)), std::move(callback));
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

  // Use AbpActionContext with skip_execution_control=true
  AbpActionContext::Options options;
  options.skip_execution_control = true;

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

void AbpController::OnGoForwardBeforeScreenshot(
    const std::string& tab_id,
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_before_path) {
  if (context) {
    context->screenshot_before_path = screenshot_before_path;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "TAB_NOT_FOUND", "Tab not found", context->start_time, 0,
          context->screenshot_before_path, "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  wc->GetController().GoForward();

  // Use centralized action completion with wait
  base::Value::Dict result;
  result.Set("status", "navigated_forward");
  CompleteActionWithScreenshot(tab_id, std::move(context), std::move(result),
                               std::move(callback));
}

void AbpController::OnGoForwardAfterScreenshot(
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  // Legacy callback - kept for compatibility but no longer used
  base::Value::Dict result;
  result.Set("status", "navigated_forward");

  if (history_controller_ && context) {
    int64_t duration_ms =
        base::Time::Now().InMillisecondsSinceUnixEpoch() - context->start_time;
    base::Value result_value(result.Clone());
    history_controller_->RecordAction(
        context->tab_id, context->action_type, context->params, &result_value,
        true, "", "", context->start_time, duration_ms,
        context->screenshot_before_path, screenshot_after_path);
  }

  SendJson(200, base::Value(std::move(result)), std::move(callback));
}

void AbpController::Screenshot(const std::string& tab_id,
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

  // Parse screenshot options from request params
  ScreenshotOptions options;

  // Get the DevToolsAgentHost ID for cursor state lookup
  // Must use GetOrCreateForTab to match what Click() uses
  auto host = content::DevToolsAgentHost::GetOrCreateForTab(wc);
  std::string host_id = host ? host->GetId() : "";

  // Get screenshot sub-object if present
  const base::Value::Dict* screenshot_params = params.FindDict("screenshot");
  if (screenshot_params) {
    if (const std::string* format = screenshot_params->FindString("format")) {
      if (*format == "png" || *format == "jpeg" || *format == "webp") {
        options.format = *format;
      }
    }
    if (auto quality = screenshot_params->FindInt("quality")) {
      options.quality = std::clamp(*quality, 1, 100);
    }
    if (const std::string* markup = screenshot_params->FindString("markup")) {
      options.markup = *markup;
    }
    if (const std::string* mouse = screenshot_params->FindString("mouse")) {
      options.mouse = *mouse;
    }
    if (auto cursor = screenshot_params->FindBool("cursor")) {
      options.cursor = *cursor;
    }
  }

  // Also check top-level params for backward compatibility
  if (const std::string* format = params.FindString("format")) {
    if (*format == "png" || *format == "jpeg" || *format == "webp") {
      options.format = *format;
    }
  }
  if (auto quality = params.FindInt("quality")) {
    options.quality = std::clamp(*quality, 1, 100);
  }
  if (auto cursor = params.FindBool("cursor")) {
    options.cursor = *cursor;
  }

  // If cursor=false, hide the virtual cursor before capturing
  if (!options.cursor) {
    content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
    if (rwhv) {
      content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
      if (rwh) {
        rwh->SetVirtualCursorVisible(false);
      }
    }
  }

  // If markup is requested, inject CSS-only outline styles (handles thousands of elements)
  if (options.markup != "none") {
    // Pure CSS approach: inject a single <style> element with selectors
    // No per-element JS iteration needed - browser CSS engine handles matching
    // O(1) JavaScript, scales to thousands of elements
    std::string css_rules;
    if (options.markup == "interactive") {
      css_rules = R"(
        a, [role='link'] { outline:2px solid #2196F3!important; outline-offset:-2px!important; }
        button, [role='button'], [onclick], [tabindex]:not([tabindex='-1']) { outline:2px solid #4CAF50!important; outline-offset:-2px!important; }
        input:not([type='hidden']) { outline:2px solid #FF9800!important; outline-offset:-2px!important; }
        select { outline:2px solid #9C27B0!important; outline-offset:-2px!important; }
        textarea, [contenteditable='true'] { outline:2px solid #795548!important; outline-offset:-2px!important; }
      )";
    } else if (options.markup == "clickable") {
      css_rules = R"(
        a, [role='link'] { outline:2px solid #2196F3!important; outline-offset:-2px!important; }
        button, [role='button'], [onclick] { outline:2px solid #4CAF50!important; outline-offset:-2px!important; }
      )";
    } else if (options.markup == "typeable") {
      css_rules = R"(
        input:not([type='hidden']):not([type='checkbox']):not([type='radio']):not([type='submit']):not([type='button']),
        textarea, [contenteditable='true'] { outline:2px solid #FF9800!important; outline-offset:-2px!important; }
      )";
    } else if (options.markup == "inputs") {
      css_rules = R"(
        input:not([type='hidden']) { outline:2px solid #FF9800!important; outline-offset:-2px!important; }
        select { outline:2px solid #9C27B0!important; outline-offset:-2px!important; }
        textarea { outline:2px solid #795548!important; outline-offset:-2px!important; }
      )";
    }

    // Synchronous style injection - no rAF needed since CSS applies immediately
    // and Page.captureScreenshot waits for rendering
    std::string script = R"(
      (function() {
        const old = document.getElementById('abp-markup-style');
        if (old) old.remove();
        const style = document.createElement('style');
        style.id = 'abp-markup-style';
        style.textContent = `)" + css_rules + R"(`;
        document.head.appendChild(style);
        return true;
      })()
    )";

    base::Value::Dict js_params;
    js_params.Set("expression", script);
    js_params.Set("returnByValue", true);

    client->SendCommand(
        "Runtime.evaluate", js_params,
        base::BindOnce(&AbpController::OnMarkupInjected,
                       weak_factory_.GetWeakPtr(), tab_id,
                       std::move(callback), options));
    return;
  }

  // No markup - check if we need to set cursor for screenshot
  if (options.mouse != "none") {
    // Get stored cursor position or use viewport center
    double cursor_x = 0;
    double cursor_y = 0;
    bool cursor_visible = false;

    if (!host_id.empty()) {
      auto it = virtual_cursor_states_.find(host_id);
      if (it != virtual_cursor_states_.end() && it->second.active) {
        cursor_x = it->second.x;
        cursor_y = it->second.y;
        cursor_visible = true;
      }
    }

    // If no cursor state, use viewport center
    if (!cursor_visible) {
      content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
      if (rwhv) {
        gfx::Size viewport_size = rwhv->GetVisibleViewportSize();
        cursor_x = viewport_size.width() / 2.0;
        cursor_y = viewport_size.height() / 2.0;
        cursor_visible = true;
      }
    }

    if (cursor_visible) {
      // Set the virtual cursor before taking the screenshot
      base::Value::Dict cursor_config;
      cursor_config.Set("x", cursor_x);
      cursor_config.Set("y", cursor_y);
      cursor_config.Set("visible", true);

      base::Value::Dict cursor_params;
      cursor_params.Set("cursorConfig", std::move(cursor_config));

      // Chain: set cursor -> take screenshot with CopyFromSurface
      client->SendCommand(
          "Overlay.setVirtualCursor", cursor_params,
          base::BindOnce(&AbpController::OnCursorSetForScreenshot,
                         weak_factory_.GetWeakPtr(), tab_id, base::Value::Dict(),
                         std::move(callback), options, wc));
      return;
    }
  }

  // CDP: Page.captureScreenshot (no cursor)
  base::Value::Dict cdp_params;
  cdp_params.Set("format", options.format);
  if (options.format != "png") {
    cdp_params.Set("quality", options.quality);
  }

  client->SendCommand(
      "Page.captureScreenshot", cdp_params,
      base::BindOnce(&AbpController::OnScreenshotResult,
                     weak_factory_.GetWeakPtr(), std::move(callback), options));
}

void AbpController::OnScreenshotResult(ResponseCallback callback,
                                       const ScreenshotOptions& options,
                                       bool success,
                                       const std::string& result) {
  if (!success) {
    SendError(500, result, std::move(callback));
    return;
  }

  // Parse the result to extract the base64 data
  auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    SendError(500, "Invalid CDP response", std::move(callback));
    return;
  }

  const std::string* data = parsed->GetDict().FindString("data");
  if (!data) {
    SendError(500, "No screenshot data", std::move(callback));
    return;
  }

  // Determine MIME type based on format
  std::string mime_type = "image/png";
  if (options.format == "jpeg") {
    mime_type = "image/jpeg";
  } else if (options.format == "webp") {
    mime_type = "image/webp";
  }

  base::Value::Dict response;
  response.Set("data", *data);
  response.Set("mimeType", mime_type);
  response.Set("format", options.format);
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::OnMarkupInjected(const std::string& tab_id,
                                     ResponseCallback callback,
                                     const ScreenshotOptions& options,
                                     bool success,
                                     const std::string& result) {
  // Markup overlay already injected by the JavaScript, now set cursor and take screenshot
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

  // If mouse is enabled, set cursor position before taking screenshot
  if (options.mouse != "none") {
    // Get stored cursor position or use viewport center
    // Must use GetOrCreateForTab to match what Click() uses
    auto host = content::DevToolsAgentHost::GetOrCreateForTab(wc);
    std::string host_id = host ? host->GetId() : "";

    double cursor_x = 0;
    double cursor_y = 0;
    bool cursor_visible = false;

    if (!host_id.empty()) {
      auto it = virtual_cursor_states_.find(host_id);
      if (it != virtual_cursor_states_.end() && it->second.active) {
        cursor_x = it->second.x;
        cursor_y = it->second.y;
        cursor_visible = true;
      }
    }

    // If no cursor state, use viewport center
    if (!cursor_visible) {
      content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
      if (rwhv) {
        gfx::Size viewport_size = rwhv->GetVisibleViewportSize();
        cursor_x = viewport_size.width() / 2.0;
        cursor_y = viewport_size.height() / 2.0;
        cursor_visible = true;
      }
    }

    if (cursor_visible) {
      // Set the virtual cursor before taking the screenshot
      base::Value::Dict cursor_config;
      cursor_config.Set("x", cursor_x);
      cursor_config.Set("y", cursor_y);
      cursor_config.Set("visible", true);

      base::Value::Dict cursor_params;
      cursor_params.Set("cursorConfig", std::move(cursor_config));

      // Store cursor position for mouse move event
      double x_copy = cursor_x;
      double y_copy = cursor_y;

      // Chain: set cursor -> send mouse move -> take screenshot
      client->SendCommand(
          "Overlay.setVirtualCursor", cursor_params,
          base::BindOnce(
              [](base::WeakPtr<AbpController> controller, AbpCdpClient* client,
                 const std::string& tab_id, double x, double y,
                 ResponseCallback cb, ScreenshotOptions opts,
                 content::WebContents* wc, bool success, const std::string& result) {
                if (!controller || !client) {
                  return;
                }

                // Also send mouseMoved event to trigger cursor detection
                base::Value::Dict move_params;
                move_params.Set("type", "mouseMoved");
                move_params.Set("x", x);
                move_params.Set("y", y);

                client->SendCommand(
                    "Input.dispatchMouseEvent", move_params,
                    base::BindOnce(
                        [](base::WeakPtr<AbpController> ctrl, const std::string& tid,
                           ResponseCallback callback, ScreenshotOptions options,
                           content::WebContents* web_contents, bool s, const std::string& r) {
                          if (!ctrl) {
                            return;
                          }
                          ctrl->OnCursorSetForScreenshot(tid, base::Value::Dict(),
                                                         std::move(callback), options,
                                                         web_contents, s, r);
                        },
                        controller, tab_id, std::move(cb), opts, wc));
              },
              weak_factory_.GetWeakPtr(), client, tab_id, x_copy, y_copy,
              std::move(callback), options, wc));
      return;
    }
  }

  // Fall back to CDP screenshot (no cursor)
  base::Value::Dict cdp_params;
  cdp_params.Set("format", options.format);
  if (options.format != "png") {
    cdp_params.Set("quality", options.quality);
  }

  client->SendCommand(
      "Page.captureScreenshot", cdp_params,
      base::BindOnce(
          [](base::WeakPtr<AbpController> controller, const std::string& tab_id,
             ResponseCallback callback, ScreenshotOptions options, bool success,
             const std::string& result) {
            if (!controller) {
              return;
            }

            // Clean up markup style
            content::WebContents* wc = controller->FindWebContents(tab_id);
            if (wc) {
              AbpCdpClient* client = controller->GetOrCreateCdpClient(wc);
              if (client) {
                base::Value::Dict cleanup_params;
                cleanup_params.Set("expression",
                    "document.getElementById('abp-markup-style')?.remove()");
                cleanup_params.Set("returnByValue", true);
                client->SendCommand("Runtime.evaluate", cleanup_params,
                                    base::BindOnce([](bool, const std::string&) {}));
              }
            }

            if (!success) {
              controller->SendError(500, result, std::move(callback));
              return;
            }

            // Parse screenshot result
            auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
            if (!parsed || !parsed->is_dict()) {
              controller->SendError(500, "Invalid CDP response", std::move(callback));
              return;
            }

            const std::string* data = parsed->GetDict().FindString("data");
            if (!data) {
              controller->SendError(500, "No screenshot data", std::move(callback));
              return;
            }

            // Build response (no marked_elements - just the screenshot)
            std::string mime_type = "image/png";
            if (options.format == "jpeg") {
              mime_type = "image/jpeg";
            } else if (options.format == "webp") {
              mime_type = "image/webp";
            }

            base::Value::Dict response;
            response.Set("data", *data);
            response.Set("mimeType", mime_type);
            response.Set("format", options.format);
            response.Set("markup", options.markup);

            controller->SendJson(200, base::Value(std::move(response)),
                                 std::move(callback));
          },
          weak_factory_.GetWeakPtr(), tab_id, std::move(callback), options));
}

void AbpController::OnCursorSetForScreenshot(const std::string& tab_id,
                                              base::Value::Dict params,
                                              ResponseCallback callback,
                                              const ScreenshotOptions& options,
                                              content::WebContents* wc,
                                              bool success,
                                              const std::string& result) {
  // Cursor set (or failed, but we continue anyway)
  // Now capture the screenshot using CopyFromSurface to include the overlay

  // Delay to allow the overlay to repaint with the cursor
  // The overlay needs time to process the cursor update and schedule a paint
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::CaptureScreenshotWithCursor,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(callback), options),
      base::Milliseconds(200));
}

void AbpController::CaptureScreenshotWithCursor(const std::string& tab_id,
                                                 ResponseCallback callback,
                                                 const ScreenshotOptions& options) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    SendError(500, "No RenderWidgetHostView for screenshot", std::move(callback));
    return;
  }

  // Use CopyFromSurface which includes the inspector overlay (with cursor)
  rwhv->CopyFromSurface(
      gfx::Rect(),   // empty = full viewport
      gfx::Size(),   // empty = native resolution
      base::Seconds(5),  // timeout
      base::BindOnce(&AbpController::OnCursorScreenshotCaptured,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(callback), options));
}

void AbpController::OnCursorScreenshotCaptured(const std::string& tab_id,
                                                ResponseCallback callback,
                                                const ScreenshotOptions& options,
                                                const content::CopyFromSurfaceResult& result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // If CopyFromSurface failed, fall back to CDP Page.captureScreenshot
  // (the cursor won't be visible but the screenshot will work)
  if (!result.has_value() || result->bitmap.empty()) {
    LOG(WARNING) << "ABP: CopyFromSurface failed (empty=" << (result.has_value() ? result->bitmap.empty() : true) << "), falling back to CDP screenshot";

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

    // Fall back to CDP Page.captureScreenshot
    base::Value::Dict cdp_params;
    cdp_params.Set("format", options.format);
    if (options.format != "png") {
      cdp_params.Set("quality", options.quality);
    }

    client->SendCommand(
        "Page.captureScreenshot", cdp_params,
        base::BindOnce(&AbpController::OnScreenshotResult,
                       weak_factory_.GetWeakPtr(), std::move(callback), options));
    return;
  }

  const SkBitmap& bitmap = result->bitmap;

  // Encode the bitmap in the requested format
  std::optional<std::vector<uint8_t>> encoded;
  std::string mime_type;

  if (options.format == "png") {
    encoded = gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, false);
    mime_type = "image/png";
  } else if (options.format == "jpeg") {
    encoded = gfx::JPEGCodec::Encode(bitmap, options.quality);
    mime_type = "image/jpeg";
  } else {  // webp
    encoded = gfx::WebpCodec::Encode(bitmap, options.quality);
    mime_type = "image/webp";
  }

  if (!encoded || encoded->empty()) {
    SendError(500, "Failed to encode screenshot", std::move(callback));
    return;
  }

  // Base64 encode the image data
  std::string base64_data = base::Base64Encode(*encoded);

  base::Value::Dict response;
  response.Set("data", base64_data);
  response.Set("mimeType", mime_type);
  response.Set("format", options.format);
  if (options.markup != "none") {
    response.Set("markup", options.markup);
  }

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::ExecuteScript(const std::string& tab_id,
                                  const base::Value::Dict& params,
                                  ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  const std::string* script = params.FindString("script");
  if (!script) {
    SendError(400, "Missing 'script' parameter", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to create CDP client", std::move(callback));
    return;
  }

  // CDP: Runtime.evaluate
  base::Value::Dict cdp_params;
  cdp_params.Set("expression", *script);
  cdp_params.Set("returnByValue", true);

  client->SendCommand(
      "Runtime.evaluate", cdp_params,
      base::BindOnce(&AbpController::OnExecuteScriptResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpController::OnExecuteScriptResult(ResponseCallback callback,
                                          bool success,
                                          const std::string& result) {
  if (!success) {
    SendError(500, result, std::move(callback));
    return;
  }

  // Parse the result
  auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    SendError(500, "Invalid CDP response", std::move(callback));
    return;
  }

  const base::Value::Dict& dict = parsed->GetDict();

  // Check for exception
  const base::Value::Dict* exception = dict.FindDict("exceptionDetails");
  if (exception) {
    const base::Value::Dict* exc = exception->FindDict("exception");
    const std::string* desc = exc ? exc->FindString("description") : nullptr;
    SendError(400, desc ? *desc : "Script exception", std::move(callback));
    return;
  }

  // Get result value
  const base::Value::Dict* cdp_result = dict.FindDict("result");
  if (cdp_result) {
    base::Value::Dict response;
    response.Set("result", cdp_result->Clone());
    SendJson(200, base::Value(std::move(response)), std::move(callback));
  } else {
    SendJson(200, base::Value(base::Value::Dict()), std::move(callback));
  }
}

void AbpController::Click(const std::string& tab_id,
                          const base::Value::Dict& params,
                          ResponseCallback callback) {
  // Validate params early
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    SendError(400, "Missing 'x' or 'y' parameter", std::move(callback));
    return;
  }

  double click_x = *x_opt;
  double click_y = *y_opt;

  // Use AbpActionContext for unified action flow:
  // Resume -> BeforeScreenshot -> Action -> Wait -> Pause -> AfterScreenshot -> Response
  AbpActionContext::Run(
      this, tab_id, "click", params,
      // Action callback - performs the actual click
      base::BindOnce(
          [](double coord_x, double coord_y, AbpActionContext* ctx) {
            // Update virtual cursor state via controller
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), coord_x, coord_y);

            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            // Set virtual cursor position via CDP overlay
            base::Value::Dict cursor_config;
            cursor_config.Set("x", coord_x);
            cursor_config.Set("y", coord_y);
            cursor_config.Set("visible", true);

            base::Value::Dict cursor_params;
            cursor_params.Set("cursorConfig", std::move(cursor_config));

            // Take a scoped_refptr to keep context alive through async calls
            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            client->SendCommand(
                "Overlay.setVirtualCursor", std::move(cursor_params),
                base::BindOnce(
                    [](double x, double y,
                       scoped_refptr<AbpActionContext> action_ctx, bool success,
                       const std::string& result) {
                      // Ignore cursor set result - proceed with click regardless
                      AbpCdpClient* cdp_client = action_ctx->client();
                      if (!cdp_client) {
                        action_ctx->OnActionError("CDP_ERROR", "CDP client lost");
                        return;
                      }

                      // Send mousePressed
                      base::Value::Dict press_params;
                      press_params.Set("type", "mousePressed");
                      press_params.Set("x", x);
                      press_params.Set("y", y);
                      press_params.Set("button", "left");
                      press_params.Set("clickCount", 1);

                      cdp_client->SendCommand(
                          "Input.dispatchMouseEvent", std::move(press_params),
                          base::BindOnce(
                              [](double rel_x, double rel_y,
                                 scoped_refptr<AbpActionContext> ctx,
                                 bool success, const std::string& result) {
                                if (!success) {
                                  ctx->OnActionError("CDP_ERROR", result);
                                  return;
                                }

                                AbpCdpClient* client = ctx->client();
                                if (!client) {
                                  ctx->OnActionError("CDP_ERROR", "CDP client lost");
                                  return;
                                }

                                // Send mouseReleased
                                base::Value::Dict release_params;
                                release_params.Set("type", "mouseReleased");
                                release_params.Set("x", rel_x);
                                release_params.Set("y", rel_y);
                                release_params.Set("button", "left");
                                release_params.Set("clickCount", 1);

                                client->SendCommand(
                                    "Input.dispatchMouseEvent", std::move(release_params),
                                    base::BindOnce(
                                        [](scoped_refptr<AbpActionContext> c,
                                           bool success,
                                           const std::string& result) {
                                          if (!success) {
                                            c->OnActionError("CDP_ERROR", result);
                                            return;
                                          }

                                          // Set result and signal action complete
                                          base::Value::Dict res;
                                          res.Set("status", "clicked");
                                          c->SetResult(std::move(res));
                                          c->OnActionDispatched();
                                        },
                                        ctx));
                              },
                              x, y, action_ctx));
                    },
                    coord_x, coord_y, ctx_ref));
          },
          click_x, click_y),
      std::move(callback));
}

void AbpController::OnClickBeforeScreenshot(
    const std::string& tab_id,
    double x,
    double y,
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_before_path) {
  if (context) {
    context->screenshot_before_path = screenshot_before_path;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "TAB_NOT_FOUND", "Tab not found", context->start_time, 0,
          context->screenshot_before_path, "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "CDP_ERROR", "Failed to create CDP client", context->start_time,
          0, context->screenshot_before_path, "");
    }
    SendError(500, "Failed to create CDP client", std::move(callback));
    return;
  }

  // If execution control is enabled, resume before the click
  auto it = execution_states_.find(tab_id);
  if (it != execution_states_.end() && it->second.debugger_enabled) {
    ResumeExecution(
        tab_id,
        base::BindOnce(
            [](base::WeakPtr<AbpController> controller, std::string tid,
               double x, double y, std::unique_ptr<ActionContext> ctx,
               ResponseCallback cb) {
              if (!controller) {
                return;
              }
              controller->DispatchClickEvent(tid, x, y, std::move(ctx),
                                             std::move(cb));
            },
            weak_factory_.GetWeakPtr(), tab_id, x, y, std::move(context),
            std::move(callback)));
    return;
  }

  // No execution control, dispatch click directly
  DispatchClickEvent(tab_id, x, y, std::move(context), std::move(callback));
}

void AbpController::DispatchClickEvent(const std::string& tab_id,
                                       double x,
                                       double y,
                                       std::unique_ptr<ActionContext> context,
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

  // CDP: Input.dispatchMouseEvent (mousePressed then mouseReleased)
  base::Value::Dict cdp_params;
  cdp_params.Set("type", "mousePressed");
  cdp_params.Set("x", x);
  cdp_params.Set("y", y);
  cdp_params.Set("button", "left");
  cdp_params.Set("clickCount", 1);

  client->SendCommand(
      "Input.dispatchMouseEvent", cdp_params,
      base::BindOnce(
          [](base::WeakPtr<AbpController> controller, std::string tab_id,
             double x, double y, std::unique_ptr<ActionContext> ctx,
             ResponseCallback cb, bool success, const std::string& result) {
            if (!controller) {
              return;
            }
            if (!success) {
              if (controller->history_controller_) {
                controller->history_controller_->RecordAction(
                    ctx->tab_id, ctx->action_type, ctx->params, nullptr, false,
                    "CDP_ERROR", result, ctx->start_time, 0,
                    ctx->screenshot_before_path, "");
              }
              controller->SendError(500, result, std::move(cb));
              return;
            }
            controller->OnClickPressedResult(tab_id, x, y, std::move(ctx),
                                             std::move(cb), success, result);
          },
          weak_factory_.GetWeakPtr(), tab_id, x, y, std::move(context),
          std::move(callback)));
}

void AbpController::OnClickPressedResult(const std::string& tab_id,
                                         double x,
                                         double y,
                                         std::unique_ptr<ActionContext> context,
                                         ResponseCallback callback,
                                         bool success,
                                         const std::string& result) {
  if (!success) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "CDP_ERROR", result, context->start_time, 0,
          context->screenshot_before_path, "");
    }
    SendError(500, result, std::move(callback));
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "TAB_NOT_FOUND", "Tab not found", context->start_time, 0,
          context->screenshot_before_path, "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "CDP_ERROR", "Failed to create CDP client", context->start_time,
          0, context->screenshot_before_path, "");
    }
    SendError(500, "Failed to create CDP client", std::move(callback));
    return;
  }

  base::Value::Dict release_params;
  release_params.Set("type", "mouseReleased");
  release_params.Set("x", x);
  release_params.Set("y", y);
  release_params.Set("button", "left");
  release_params.Set("clickCount", 1);

  client->SendCommand(
      "Input.dispatchMouseEvent", release_params,
      base::BindOnce(&AbpController::OnClickResult, weak_factory_.GetWeakPtr(),
                     std::move(context), std::move(callback)));
}

void AbpController::OnClickResult(std::unique_ptr<ActionContext> context,
                                  ResponseCallback callback,
                                  bool success,
                                  const std::string& result) {
  if (!success) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "CDP_ERROR", result, context->start_time, 0,
          context->screenshot_before_path, "");
    }
    SendError(500, result, std::move(callback));
    return;
  }

  // Use centralized action completion with wait
  std::string tab_id = context ? context->tab_id : "";
  base::Value::Dict response;
  response.Set("status", "clicked");
  CompleteActionWithScreenshot(tab_id, std::move(context), std::move(response),
                               std::move(callback));
}

void AbpController::OnClickAfterScreenshot(
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  // Legacy callback - kept for compatibility but no longer used
  base::Value::Dict response;
  response.Set("status", "clicked");

  if (history_controller_ && context) {
    int64_t duration_ms =
        base::Time::Now().InMillisecondsSinceUnixEpoch() - context->start_time;
    base::Value result_value(response.Clone());
    history_controller_->RecordAction(
        context->tab_id, context->action_type, context->params, &result_value,
        true, "", "", context->start_time, duration_ms,
        context->screenshot_before_path, screenshot_after_path);
  }

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::Type(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  // Validate params early
  const std::string* text = params.FindString("text");
  if (!text) {
    SendError(400, "Missing 'text' parameter", std::move(callback));
    return;
  }

  std::string text_copy = *text;

  // Use AbpActionContext for unified action flow:
  // Resume -> BeforeScreenshot -> Action -> Wait -> Pause -> AfterScreenshot -> Response
  AbpActionContext::Run(
      this, tab_id, "type", params,
      // Action callback - performs the actual type
      base::BindOnce(
          [](std::string text, AbpActionContext* ctx) {
            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            // Take a scoped_refptr to keep context alive through async call
            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            // CDP: Input.insertText - simpler than key events
            base::Value::Dict cdp_params;
            cdp_params.Set("text", text);

            client->SendCommand(
                "Input.insertText", std::move(cdp_params),
                base::BindOnce(
                    [](scoped_refptr<AbpActionContext> action_ctx, bool success,
                       const std::string& result) {
                      if (!success) {
                        action_ctx->OnActionError("CDP_ERROR", result);
                        return;
                      }

                      // Set result and signal action complete
                      base::Value::Dict res;
                      res.Set("status", "typed");
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    ctx_ref));
          },
          std::move(text_copy)),
      std::move(callback));
}

void AbpController::Move(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  // Validate params early
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    SendError(400, "Missing 'x' or 'y' parameter", std::move(callback));
    return;
  }

  double move_x = *x_opt;
  double move_y = *y_opt;

  // Use AbpActionContext for unified action flow
  AbpActionContext::Run(
      this, tab_id, "move", params,
      // Action callback - performs the cursor move
      base::BindOnce(
          [](double coord_x, double coord_y, AbpActionContext* ctx) {
            // Update virtual cursor state via controller
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), coord_x, coord_y);

            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            // Set virtual cursor position via CDP overlay
            base::Value::Dict cursor_config;
            cursor_config.Set("x", coord_x);
            cursor_config.Set("y", coord_y);
            cursor_config.Set("visible", true);

            base::Value::Dict cursor_params;
            cursor_params.Set("cursorConfig", std::move(cursor_config));

            // Take a scoped_refptr to keep context alive through async calls
            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            client->SendCommand(
                "Overlay.setVirtualCursor", std::move(cursor_params),
                base::BindOnce(
                    [](double x, double y,
                       scoped_refptr<AbpActionContext> action_ctx, bool success,
                       const std::string& result) {
                      // Ignore cursor set result - proceed regardless
                      AbpCdpClient* cdp_client = action_ctx->client();
                      if (!cdp_client) {
                        action_ctx->OnActionError("CDP_ERROR", "CDP client lost");
                        return;
                      }

                      // Send mouseMoved event
                      base::Value::Dict move_params;
                      move_params.Set("type", "mouseMoved");
                      move_params.Set("x", x);
                      move_params.Set("y", y);

                      cdp_client->SendCommand(
                          "Input.dispatchMouseEvent", std::move(move_params),
                          base::BindOnce(
                              [](double final_x, double final_y,
                                 scoped_refptr<AbpActionContext> ctx,
                                 bool success, const std::string& result) {
                                if (!success) {
                                  ctx->OnActionError("CDP_ERROR", result);
                                  return;
                                }

                                // Set result and signal action complete
                                base::Value::Dict res;
                                res.Set("status", "moved");
                                res.Set("x", final_x);
                                res.Set("y", final_y);
                                ctx->SetResult(std::move(res));
                                ctx->OnActionDispatched();
                              },
                              x, y, action_ctx));
                    },
                    coord_x, coord_y, ctx_ref));
          },
          move_x, move_y),
      std::move(callback));
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

  // Use AbpActionContext WITHOUT skip_execution_control
  // This allows JavaScript to run during the wait period (for animations, etc.)
  // Flow: Resume V8 -> Wait -> Pause V8 -> Screenshot
  AbpActionContext::Run(
      this, tab_id, "wait", params,
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

void AbpController::Scroll(const std::string& tab_id,
                           const base::Value::Dict& params,
                           ResponseCallback callback) {
  // Get scroll coordinates (default to center of viewport if not specified)
  double x = params.FindDouble("x").value_or(500);
  double y = params.FindDouble("y").value_or(500);
  double delta_x = params.FindDouble("delta_x").value_or(0);
  double delta_y = params.FindDouble("delta_y").value_or(0);

  if (delta_x == 0 && delta_y == 0) {
    SendError(400, "At least one of 'delta_x' or 'delta_y' must be non-zero",
              std::move(callback));
    return;
  }

  // Use AbpActionContext for unified action flow
  AbpActionContext::Run(
      this, tab_id, "scroll", params,
      // Action callback - performs the scroll
      base::BindOnce(
          [](double scroll_x, double scroll_y, double dx, double dy,
             AbpActionContext* ctx) {
            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            // Take a scoped_refptr to keep context alive
            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            // Send mouseWheel event via CDP
            base::Value::Dict wheel_params;
            wheel_params.Set("type", "mouseWheel");
            wheel_params.Set("x", scroll_x);
            wheel_params.Set("y", scroll_y);
            wheel_params.Set("deltaX", dx);
            wheel_params.Set("deltaY", dy);

            client->SendCommand(
                "Input.dispatchMouseEvent", std::move(wheel_params),
                base::BindOnce(
                    [](double final_x, double final_y, double final_dx,
                       double final_dy, scoped_refptr<AbpActionContext> action_ctx,
                       bool success, const std::string& result) {
                      if (!success) {
                        action_ctx->OnActionError("CDP_ERROR", result);
                        return;
                      }

                      base::Value::Dict res;
                      res.Set("status", "scrolled");
                      res.Set("x", final_x);
                      res.Set("y", final_y);
                      res.Set("delta_x", final_dx);
                      res.Set("delta_y", final_dy);
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    scroll_x, scroll_y, dx, dy, ctx_ref));
          },
          x, y, delta_x, delta_y),
      std::move(callback));
}

void AbpController::KeyPress(const std::string& tab_id,
                             const base::Value::Dict& params,
                             ResponseCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    SendError(400, "Missing 'key' parameter", std::move(callback));
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

  // Use AbpActionContext for unified action flow
  AbpActionContext::Run(
      this, tab_id, "key_press", params,
      base::BindOnce(
          [](std::string pressed_key, std::vector<std::string> mods,
             AbpActionContext* ctx) {
            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            KeyInfo key_info = GetKeyInfo(pressed_key);
            int mod_flags = ModifiersToFlags(mods);

            // Helper to send a key event
            auto send_key_event = [](AbpCdpClient* cdp_client,
                                     const std::string& type,
                                     const KeyInfo& info, int modifiers,
                                     base::OnceCallback<void(bool, const std::string&)>
                                         callback) {
              base::Value::Dict key_params;
              key_params.Set("type", type);
              key_params.Set("key", info.key);
              key_params.Set("code", info.code);
              key_params.Set("windowsVirtualKeyCode", info.windows_virtual_key);
              key_params.Set("nativeVirtualKeyCode", info.native_virtual_key);
              key_params.Set("modifiers", modifiers);
              cdp_client->SendCommand("Input.dispatchKeyEvent",
                                      std::move(key_params), std::move(callback));
            };

            // For shortcuts with modifiers: press modifiers down, press key, release key, release modifiers
            // For simple key press: just keyDown + keyUp

            if (mods.empty()) {
              // Simple key press: keyDown then keyUp
              send_key_event(
                  client, "keyDown", key_info, mod_flags,
                  base::BindOnce(
                      [](KeyInfo info, int flags, AbpCdpClient* cdp_client,
                         scoped_refptr<AbpActionContext> action_ctx, bool success,
                         const std::string& result) {
                        if (!success) {
                          action_ctx->OnActionError("CDP_ERROR", result);
                          return;
                        }

                        // Now send keyUp
                        base::Value::Dict up_params;
                        up_params.Set("type", "keyUp");
                        up_params.Set("key", info.key);
                        up_params.Set("code", info.code);
                        up_params.Set("windowsVirtualKeyCode", info.windows_virtual_key);
                        up_params.Set("nativeVirtualKeyCode", info.native_virtual_key);
                        up_params.Set("modifiers", flags);

                        cdp_client->SendCommand(
                            "Input.dispatchKeyEvent", std::move(up_params),
                            base::BindOnce(
                                [](std::string key_name,
                                   scoped_refptr<AbpActionContext> ctx, bool success,
                                   const std::string& result) {
                                  if (!success) {
                                    ctx->OnActionError("CDP_ERROR", result);
                                    return;
                                  }

                                  base::Value::Dict res;
                                  res.Set("status", "pressed");
                                  res.Set("key", key_name);
                                  ctx->SetResult(std::move(res));
                                  ctx->OnActionDispatched();
                                },
                                info.key, action_ctx));
                      },
                      key_info, mod_flags, client, ctx_ref));
            } else {
              // Shortcut: need to press modifiers first, then key, then release in reverse
              // For simplicity, we'll send all modifier keyDowns, then main key down+up, then modifier keyUps

              // This is a bit complex - we need to chain multiple CDP calls
              // Let's do it step by step using a state machine approach

              // First, press all modifier keys down
              struct ShortcutState {
                std::vector<std::string> modifiers;
                KeyInfo main_key;
                int mod_flags;
                size_t mod_index = 0;
                raw_ptr<AbpCdpClient> client;
                scoped_refptr<AbpActionContext> ctx;

                void PressNextModifier() {
                  if (mod_index < modifiers.size()) {
                    KeyInfo mod_info = GetKeyInfo(modifiers[mod_index]);
                    mod_index++;

                    base::Value::Dict params;
                    params.Set("type", "keyDown");
                    params.Set("key", mod_info.key);
                    params.Set("code", mod_info.code);
                    params.Set("windowsVirtualKeyCode", mod_info.windows_virtual_key);
                    params.Set("nativeVirtualKeyCode", mod_info.native_virtual_key);
                    // Modifiers accumulate as we press them
                    int current_mods = 0;
                    for (size_t i = 0; i < mod_index; i++) {
                      KeyInfo ki = GetKeyInfo(modifiers[i]);
                      current_mods |= ki.modifier_flag;
                    }
                    params.Set("modifiers", current_mods);

                    client->SendCommand(
                        "Input.dispatchKeyEvent", std::move(params),
                        base::BindOnce(
                            [](ShortcutState* state, bool success,
                               const std::string& result) {
                              if (!success) {
                                state->ctx->OnActionError("CDP_ERROR", result);
                                delete state;
                                return;
                              }
                              state->PressNextModifier();
                            },
                            base::Unretained(this)));
                  } else {
                    // All modifiers pressed, now press the main key
                    PressMainKey();
                  }
                }

                void PressMainKey() {
                  base::Value::Dict params;
                  params.Set("type", "keyDown");
                  params.Set("key", main_key.key);
                  params.Set("code", main_key.code);
                  params.Set("windowsVirtualKeyCode", main_key.windows_virtual_key);
                  params.Set("nativeVirtualKeyCode", main_key.native_virtual_key);
                  params.Set("modifiers", mod_flags);

                  client->SendCommand(
                      "Input.dispatchKeyEvent", std::move(params),
                      base::BindOnce(
                          [](ShortcutState* state, bool success,
                             const std::string& result) {
                            if (!success) {
                              state->ctx->OnActionError("CDP_ERROR", result);
                              delete state;
                              return;
                            }
                            state->ReleaseMainKey();
                          },
                          base::Unretained(this)));
                }

                void ReleaseMainKey() {
                  base::Value::Dict params;
                  params.Set("type", "keyUp");
                  params.Set("key", main_key.key);
                  params.Set("code", main_key.code);
                  params.Set("windowsVirtualKeyCode", main_key.windows_virtual_key);
                  params.Set("nativeVirtualKeyCode", main_key.native_virtual_key);
                  params.Set("modifiers", mod_flags);

                  client->SendCommand(
                      "Input.dispatchKeyEvent", std::move(params),
                      base::BindOnce(
                          [](ShortcutState* state, bool success,
                             const std::string& result) {
                            if (!success) {
                              state->ctx->OnActionError("CDP_ERROR", result);
                              delete state;
                              return;
                            }
                            state->mod_index = state->modifiers.size();
                            state->ReleaseNextModifier();
                          },
                          base::Unretained(this)));
                }

                void ReleaseNextModifier() {
                  if (mod_index > 0) {
                    mod_index--;
                    KeyInfo mod_info = GetKeyInfo(modifiers[mod_index]);

                    // Calculate remaining modifiers
                    int remaining_mods = 0;
                    for (size_t i = 0; i < mod_index; i++) {
                      KeyInfo ki = GetKeyInfo(modifiers[i]);
                      remaining_mods |= ki.modifier_flag;
                    }

                    base::Value::Dict params;
                    params.Set("type", "keyUp");
                    params.Set("key", mod_info.key);
                    params.Set("code", mod_info.code);
                    params.Set("windowsVirtualKeyCode", mod_info.windows_virtual_key);
                    params.Set("nativeVirtualKeyCode", mod_info.native_virtual_key);
                    params.Set("modifiers", remaining_mods);

                    client->SendCommand(
                        "Input.dispatchKeyEvent", std::move(params),
                        base::BindOnce(
                            [](ShortcutState* state, bool success,
                               const std::string& result) {
                              if (!success) {
                                state->ctx->OnActionError("CDP_ERROR", result);
                                delete state;
                                return;
                              }
                              state->ReleaseNextModifier();
                            },
                            base::Unretained(this)));
                  } else {
                    // All done!
                    base::Value::Dict res;
                    res.Set("status", "pressed");
                    res.Set("key", main_key.key);
                    base::Value::List mod_list;
                    for (const auto& m : modifiers) {
                      mod_list.Append(m);
                    }
                    res.Set("modifiers", std::move(mod_list));
                    ctx->SetResult(std::move(res));
                    ctx->OnActionDispatched();
                    delete this;
                  }
                }
              };

              auto* state = new ShortcutState();
              state->modifiers = std::move(mods);
              state->main_key = key_info;
              state->mod_flags = mod_flags;
              state->client = client;
              state->ctx = ctx_ref;
              state->PressNextModifier();
            }
          },
          std::move(key_copy), std::move(modifiers)),
      std::move(callback));
}

void AbpController::KeyDown(const std::string& tab_id,
                            const base::Value::Dict& params,
                            ResponseCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    SendError(400, "Missing 'key' parameter", std::move(callback));
    return;
  }

  std::string key_copy = *key;

  // Use AbpActionContext for unified action flow
  AbpActionContext::Run(
      this, tab_id, "key_down", params,
      base::BindOnce(
          [](std::string pressed_key, AbpController* controller,
             AbpActionContext* ctx) {
            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            KeyInfo key_info = GetKeyInfo(pressed_key);

            // Track the held key
            auto& held_state = controller->held_keys_state_[ctx->tab_id()];
            held_state.held_keys.insert(pressed_key);
            if (key_info.is_modifier) {
              held_state.current_modifiers |= key_info.modifier_flag;
            }

            int current_mods = held_state.current_modifiers;

            base::Value::Dict key_params;
            key_params.Set("type", "keyDown");
            key_params.Set("key", key_info.key);
            key_params.Set("code", key_info.code);
            key_params.Set("windowsVirtualKeyCode", key_info.windows_virtual_key);
            key_params.Set("nativeVirtualKeyCode", key_info.native_virtual_key);
            key_params.Set("modifiers", current_mods);

            client->SendCommand(
                "Input.dispatchKeyEvent", std::move(key_params),
                base::BindOnce(
                    [](std::string key_name, scoped_refptr<AbpActionContext> action_ctx,
                       bool success, const std::string& result) {
                      if (!success) {
                        action_ctx->OnActionError("CDP_ERROR", result);
                        return;
                      }

                      base::Value::Dict res;
                      res.Set("status", "key_down");
                      res.Set("key", key_name);
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    pressed_key, ctx_ref));
          },
          std::move(key_copy), this),
      std::move(callback));
}

void AbpController::KeyUp(const std::string& tab_id,
                          const base::Value::Dict& params,
                          ResponseCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    SendError(400, "Missing 'key' parameter", std::move(callback));
    return;
  }

  std::string key_copy = *key;

  // Use AbpActionContext for unified action flow
  AbpActionContext::Run(
      this, tab_id, "key_up", params,
      base::BindOnce(
          [](std::string released_key, AbpController* controller,
             AbpActionContext* ctx) {
            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            KeyInfo key_info = GetKeyInfo(released_key);

            // Update held key tracking
            auto& held_state = controller->held_keys_state_[ctx->tab_id()];
            held_state.held_keys.erase(released_key);
            if (key_info.is_modifier) {
              held_state.current_modifiers &= ~key_info.modifier_flag;
            }

            int current_mods = held_state.current_modifiers;

            base::Value::Dict key_params;
            key_params.Set("type", "keyUp");
            key_params.Set("key", key_info.key);
            key_params.Set("code", key_info.code);
            key_params.Set("windowsVirtualKeyCode", key_info.windows_virtual_key);
            key_params.Set("nativeVirtualKeyCode", key_info.native_virtual_key);
            key_params.Set("modifiers", current_mods);

            client->SendCommand(
                "Input.dispatchKeyEvent", std::move(key_params),
                base::BindOnce(
                    [](std::string key_name, scoped_refptr<AbpActionContext> action_ctx,
                       bool success, const std::string& result) {
                      if (!success) {
                        action_ctx->OnActionError("CDP_ERROR", result);
                        return;
                      }

                      base::Value::Dict res;
                      res.Set("status", "key_up");
                      res.Set("key", key_name);
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    released_key, ctx_ref));
          },
          std::move(key_copy), this),
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
          history_controller_->RecordAction(tab_id, "activate_tab", params,
                                            &result_value, true, "", "",
                                            start_time, duration_ms, "", "");
        }

        SendJson(200, base::Value(std::move(result)), std::move(callback));
        return;
      }
    }
  }

  if (history_controller_) {
    history_controller_->RecordAction(tab_id, "activate_tab", params, nullptr,
                                      false, "TAB_NOT_FOUND", "Tab not found",
                                      start_time, 0, "", "");
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
      history_controller_->RecordAction(tab_id, "stop_loading", params, nullptr,
                                        false, "TAB_NOT_FOUND", "Tab not found",
                                        start_time, 0, "", "");
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
    history_controller_->RecordAction(tab_id, "stop_loading", params,
                                      &result_value, true, "", "",
                                      start_time, duration_ms, "", "");
  }

  SendJson(200, base::Value(std::move(result)), std::move(callback));
}

void AbpController::OnTypeBeforeScreenshot(
    const std::string& tab_id,
    const std::string& text,
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_before_path) {
  if (context) {
    context->screenshot_before_path = screenshot_before_path;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "TAB_NOT_FOUND", "Tab not found", context->start_time, 0,
          context->screenshot_before_path, "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(
          context->tab_id, context->action_type, context->params, nullptr,
          false, "CDP_ERROR", "Failed to create CDP client", context->start_time,
          0, context->screenshot_before_path, "");
    }
    SendError(500, "Failed to create CDP client", std::move(callback));
    return;
  }

  // If execution control is enabled, resume before the type action
  auto it = execution_states_.find(tab_id);
  if (it != execution_states_.end() && it->second.debugger_enabled) {
    ResumeExecution(
        tab_id,
        base::BindOnce(
            [](base::WeakPtr<AbpController> controller, std::string tid,
               std::string txt, std::unique_ptr<ActionContext> ctx,
               ResponseCallback cb) {
              if (!controller) {
                return;
              }
              controller->DispatchTypeEvent(tid, txt, std::move(ctx),
                                            std::move(cb));
            },
            weak_factory_.GetWeakPtr(), tab_id, text, std::move(context),
            std::move(callback)));
    return;
  }

  // No execution control, dispatch type directly
  DispatchTypeEvent(tab_id, text, std::move(context), std::move(callback));
}

void AbpController::DispatchTypeEvent(const std::string& tab_id,
                                      const std::string& text,
                                      std::unique_ptr<ActionContext> context,
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

  // CDP: Input.insertText - simpler than key events
  base::Value::Dict cdp_params;
  cdp_params.Set("text", text);

  client->SendCommand(
      "Input.insertText", cdp_params,
      base::BindOnce(&AbpController::OnTypeResult, weak_factory_.GetWeakPtr(),
                     std::move(context), std::move(callback)));
}

void AbpController::OnTypeResult(std::unique_ptr<ActionContext> context,
                                 ResponseCallback callback,
                                 bool success,
                                 const std::string& result) {
  if (!success) {
    if (history_controller_ && context) {
      history_controller_->RecordAction(context->tab_id, context->action_type,
                                        context->params, nullptr, false,
                                        "CDP_ERROR", result, context->start_time,
                                        0, context->screenshot_before_path, "");
    }
    SendError(500, result, std::move(callback));
    return;
  }

  // Use centralized action completion with wait
  std::string tab_id = context ? context->tab_id : "";
  base::Value::Dict response;
  response.Set("status", "typed");
  CompleteActionWithScreenshot(tab_id, std::move(context), std::move(response),
                               std::move(callback));
}

void AbpController::OnTypeAfterScreenshot(
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  // Legacy callback - kept for compatibility but no longer used
  base::Value::Dict response;
  response.Set("status", "typed");

  if (history_controller_ && context) {
    int64_t duration_ms =
        base::Time::Now().InMillisecondsSinceUnixEpoch() - context->start_time;
    base::Value result_value(response.Clone());
    history_controller_->RecordAction(
        context->tab_id, context->action_type, context->params, &result_value,
        true, "", "", context->start_time, duration_ms,
        context->screenshot_before_path, screenshot_after_path);
  }

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

content::WebContents* AbpController::FindWebContents(
    const std::string& tab_id) {
  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      if (host->GetId() == tab_id) {
        return wc;
      }
    }
  }
  return nullptr;
}

AbpCdpClient* AbpController::GetOrCreateCdpClient(content::WebContents* wc) {
  auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
  const std::string& id = host->GetId();

  auto it = cdp_clients_.find(id);
  if (it != cdp_clients_.end()) {
    return it->second.get();
  }

  auto client = std::make_unique<AbpCdpClient>(host);
  AbpCdpClient* raw_ptr = client.get();

  // Set up event listener to route CDP events to the event collector and handle dialogs
  std::string tab_id = id;
  raw_ptr->SetEventListener(base::BindRepeating(
      [](base::WeakPtr<AbpController> controller, std::string tab,
         const std::string& method, const base::Value::Dict& params) {
        if (!controller) {
          return;
        }
        // Route to event collector
        if (controller->event_collector_) {
          controller->event_collector_->OnCdpEvent(tab, method, params);
        }
        // Handle dialog events for pending_dialogs_ tracking
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
      },
      weak_factory_.GetWeakPtr(), tab_id));

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

  cdp_clients_[id] = std::move(client);
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
      content::DevToolsAgentHost::GetOrCreateForTab(wc);
  if (host) {
    VirtualCursorState& state = virtual_cursor_states_[host->GetId()];
    state.active = true;
    state.x = x;
    state.y = y;
  }
}

void AbpController::SetVirtualCursorViaMojo(content::WebContents* wc,
                                             float x,
                                             float y,
                                             bool visible) {
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

  rwh->SetVirtualCursorEnabled(enabled);
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
    LOG(WARNING) << "ABP: Tab not found for execution control: " << tab_id;
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    LOG(WARNING) << "ABP: Failed to create CDP client for execution control";
    std::move(then).Run();
    return;
  }

  // Check if already enabled
  auto& state = execution_states_[tab_id];
  if (state.debugger_enabled && state.virtual_time_enabled) {
    std::move(then).Run();
    return;
  }

  // Step 1: Enable Debugger domain
  base::Value::Dict params;
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
  if (!success) {
    LOG(WARNING) << "ABP: Debugger.enable failed: " << result;
    std::move(then).Run();
    return;
  }

  auto& state = execution_states_[tab_id];
  state.debugger_enabled = true;

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

  // Step 2: Enable virtual time with initial pause
  base::Value::Dict params;
  params.Set("policy", "pause");
  if (initial_virtual_time.has_value()) {
    params.Set("initialVirtualTime", *initial_virtual_time);
  }

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
  if (!success) {
    LOG(WARNING) << "ABP: setVirtualTimePolicy failed: " << result;
    std::move(then).Run();
    return;
  }

  auto& state = execution_states_[tab_id];
  state.virtual_time_enabled = true;
  state.paused = true;  // Started in paused state

  // Parse the result to get virtualTimeTicksBase
  auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
  if (parsed && parsed->is_dict()) {
    auto ticks_base = parsed->GetDict().FindDouble("virtualTimeTicksBase");
    if (ticks_base) {
      state.virtual_time_base_ticks_ms = *ticks_base;
    }
  }

  LOG(INFO) << "ABP: Execution control enabled for tab " << tab_id
            << ", virtualTimeTicksBase=" << state.virtual_time_base_ticks_ms;
  std::move(then).Run();
}

void AbpController::ResumeExecution(const std::string& tab_id,
                                    base::OnceClosure then) {
  auto it = execution_states_.find(tab_id);
  if (it == execution_states_.end() || !it->second.debugger_enabled) {
    // Execution control not enabled for this tab, just proceed
    std::move(then).Run();
    return;
  }

  ExecutionState& state = it->second;
  if (!state.paused) {
    // Already resumed
    std::move(then).Run();
    return;
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

  // Step 1: Resume debugger
  base::Value::Dict params;
  client->SendCommand(
      "Debugger.resume", params,
      base::BindOnce(&AbpController::OnDebuggerResumed,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
}

void AbpController::OnDebuggerResumed(const std::string& tab_id,
                                      base::OnceClosure then,
                                      bool success,
                                      const std::string& result) {
  // Debugger.resume may return error if not actually paused, which is fine
  if (!success) {
    LOG(INFO) << "ABP: Debugger.resume: " << result << " (may not have been paused)";
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

  // Step 2: Resume virtual time (advance policy)
  base::Value::Dict params;
  params.Set("policy", "advance");

  client->SendCommand(
      "Emulation.setVirtualTimePolicy", params,
      base::BindOnce(&AbpController::OnVirtualTimeResumed,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
}

void AbpController::OnVirtualTimeResumed(const std::string& tab_id,
                                         base::OnceClosure then,
                                         bool success,
                                         const std::string& result) {
  if (!success) {
    LOG(WARNING) << "ABP: setVirtualTimePolicy(advance) failed: " << result;
  }

  auto it = execution_states_.find(tab_id);
  if (it != execution_states_.end()) {
    it->second.paused = false;
  }

  LOG(INFO) << "ABP: Execution resumed for tab " << tab_id;
  std::move(then).Run();
}

void AbpController::PauseExecution(const std::string& tab_id,
                                   base::OnceClosure then) {
  auto it = execution_states_.find(tab_id);
  if (it == execution_states_.end() || !it->second.debugger_enabled) {
    // Execution control not enabled for this tab, just proceed
    std::move(then).Run();
    return;
  }

  ExecutionState& state = it->second;
  if (state.paused) {
    // Already paused
    std::move(then).Run();
    return;
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

  // Step 1: Pause virtual time first (freeze time before halting JS)
  base::Value::Dict params;
  params.Set("policy", "pause");

  client->SendCommand(
      "Emulation.setVirtualTimePolicy", params,
      base::BindOnce(&AbpController::OnVirtualTimePaused,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
}

void AbpController::OnVirtualTimePaused(const std::string& tab_id,
                                        base::OnceClosure then,
                                        bool success,
                                        const std::string& result) {
  if (!success) {
    LOG(WARNING) << "ABP: setVirtualTimePolicy(pause) failed: " << result;
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

  // Step 2: Pause debugger (halt JS)
  base::Value::Dict params;
  client->SendCommand(
      "Debugger.pause", params,
      base::BindOnce(&AbpController::OnDebuggerPaused,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
}

void AbpController::OnDebuggerPaused(const std::string& tab_id,
                                     base::OnceClosure then,
                                     bool success,
                                     const std::string& result) {
  if (!success) {
    LOG(WARNING) << "ABP: Debugger.pause failed: " << result;
  }

  auto it = execution_states_.find(tab_id);
  if (it != execution_states_.end()) {
    it->second.paused = true;
  }

  LOG(INFO) << "ABP: Execution paused for tab " << tab_id;
  std::move(then).Run();
}

void AbpController::GetExecutionState(const std::string& tab_id,
                                      ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  base::Value::Dict response;

  auto it = execution_states_.find(tab_id);
  if (it != execution_states_.end()) {
    const ExecutionState& state = it->second;
    response.Set("enabled", state.debugger_enabled && state.virtual_time_enabled);
    response.Set("paused", state.paused);
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

  auto paused = params.FindBool("paused");
  if (!paused.has_value()) {
    SendError(400, "Missing 'paused' parameter", std::move(callback));
    return;
  }

  // Check if execution control is enabled for this tab
  auto it = execution_states_.find(tab_id);
  if (it == execution_states_.end() || !it->second.debugger_enabled) {
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
// Centralized action completion with wait
// =============================================================================

void AbpController::CompleteActionWithScreenshot(
    const std::string& tab_id,
    std::unique_ptr<ActionContext> context,
    base::Value::Dict result,
    ResponseCallback callback) {
  if (!context) {
    // No context, just send response without recording
    SendJson(200, base::Value(std::move(result)), std::move(callback));
    return;
  }

  // If execution control is enabled, pause before waiting/screenshot
  auto it = execution_states_.find(tab_id);
  if (it != execution_states_.end() && it->second.debugger_enabled) {
    PauseExecution(
        tab_id,
        base::BindOnce(&AbpController::OnWaitCompleteForAction,
                       weak_factory_.GetWeakPtr(), tab_id, std::move(context),
                       std::move(result), std::move(callback)));
    return;
  }

  // Wait for action_complete conditions, then take screenshot
  WaitForActionComplete(
      tab_id,
      base::BindOnce(&AbpController::OnWaitCompleteForAction,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(context),
                     std::move(result), std::move(callback)));
}

void AbpController::OnWaitCompleteForAction(
    const std::string& tab_id,
    std::unique_ptr<ActionContext> context,
    base::Value::Dict result,
    ResponseCallback callback) {
  // Take the after screenshot
  CaptureScreenshotForHistory(
      tab_id, context->start_time, false,
      base::BindOnce(&AbpController::OnActionScreenshotCaptured,
                     weak_factory_.GetWeakPtr(), std::move(context),
                     std::move(result), std::move(callback)));
}

void AbpController::OnActionScreenshotCaptured(
    std::unique_ptr<ActionContext> context,
    base::Value::Dict result,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  // Record to history
  if (history_controller_ && context) {
    int64_t duration_ms =
        base::Time::Now().InMillisecondsSinceUnixEpoch() - context->start_time;
    base::Value result_value(result.Clone());
    history_controller_->RecordAction(
        context->tab_id, context->action_type, context->params, &result_value,
        true, "", "", context->start_time, duration_ms,
        context->screenshot_before_path, screenshot_after_path);
  }

  // Send response
  SendJson(200, base::Value(std::move(result)), std::move(callback));
}

// =============================================================================
// Action complete wait implementation
// =============================================================================

// Constants for action_complete wait
namespace {
constexpr base::TimeDelta kMinWaitTime = base::Milliseconds(500);
constexpr base::TimeDelta kNetworkIdleTime = base::Milliseconds(500);
constexpr base::TimeDelta kNetworkIdleCheckInterval = base::Milliseconds(100);
constexpr base::TimeDelta kWaitTimeout = base::Seconds(30);
constexpr int kNetworkIdleMaxConnections = 2;  // networkidle2
}  // namespace

void AbpController::WaitForActionComplete(const std::string& tab_id,
                                          base::OnceClosure on_complete) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    // Tab not found, call callback immediately
    std::move(on_complete).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
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

  // For pages that are already loaded, set load events as fired
  // We'll still wait for network idle and min time
  if (!wc->IsLoading()) {
    waiter->load_fired = true;
    waiter->dom_content_loaded_fired = true;
  }

  action_waiters_[tab_id] = std::move(waiter);

  // Set up event listener for CDP events
  client->SetEventListener(base::BindRepeating(
      &AbpController::OnCdpEventForWait, weak_factory_.GetWeakPtr(), tab_id));

  // Enable Network and Page domains for events
  base::Value::Dict empty_params;
  client->SendCommand("Network.enable", empty_params,
                      base::BindOnce([](bool, const std::string&) {}));
  client->SendCommand("Page.enable", empty_params,
                      base::BindOnce([](bool, const std::string&) {}));

  // Start minimum wait timer
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnMinWaitTimeElapsed,
                     weak_factory_.GetWeakPtr(), tab_id),
      kMinWaitTime);

  // Start network idle check timer
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnNetworkIdleCheck,
                     weak_factory_.GetWeakPtr(), tab_id),
      kNetworkIdleCheckInterval);

  // Start timeout timer
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnWaitTimeout,
                     weak_factory_.GetWeakPtr(), tab_id),
      kWaitTimeout);
}

void AbpController::OnCdpEventForWait(const std::string& tab_id,
                                      const std::string& method,
                                      const base::Value::Dict& params) {
  auto it = action_waiters_.find(tab_id);
  if (it == action_waiters_.end()) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.get();

  // Track network events
  if (method == "Network.requestWillBeSent") {
    waiter->active_requests++;
    waiter->last_network_activity = base::TimeTicks::Now();
    waiter->network_idle = false;
  } else if (method == "Network.loadingFinished" ||
             method == "Network.loadingFailed") {
    if (waiter->active_requests > 0) {
      waiter->active_requests--;
    }
    waiter->last_network_activity = base::TimeTicks::Now();
  }

  // Track page load events
  if (method == "Page.loadEventFired") {
    waiter->load_fired = true;
    CheckActionCompleteConditions(tab_id);
  } else if (method == "Page.domContentEventFired") {
    waiter->dom_content_loaded_fired = true;
    CheckActionCompleteConditions(tab_id);
  }
}

void AbpController::OnMinWaitTimeElapsed(const std::string& tab_id) {
  auto it = action_waiters_.find(tab_id);
  if (it == action_waiters_.end()) {
    return;
  }

  it->second->min_time_elapsed = true;
  CheckActionCompleteConditions(tab_id);
}

void AbpController::OnNetworkIdleCheck(const std::string& tab_id) {
  auto it = action_waiters_.find(tab_id);
  if (it == action_waiters_.end()) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.get();

  // Check if network has been idle (≤2 connections) for kNetworkIdleTime
  if (waiter->active_requests <= kNetworkIdleMaxConnections) {
    base::TimeDelta idle_duration =
        base::TimeTicks::Now() - waiter->last_network_activity;
    if (idle_duration >= kNetworkIdleTime) {
      waiter->network_idle = true;
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

void AbpController::OnWaitTimeout(const std::string& tab_id) {
  auto it = action_waiters_.find(tab_id);
  if (it == action_waiters_.end()) {
    return;
  }

  LOG(WARNING) << "ABP: action_complete wait timed out for tab " << tab_id;

  // Force complete
  std::unique_ptr<ActionCompleteWaiter> waiter = std::move(it->second);
  action_waiters_.erase(it);

  // Clear event listener
  content::WebContents* wc = FindWebContents(tab_id);
  if (wc) {
    AbpCdpClient* client = GetOrCreateCdpClient(wc);
    if (client) {
      client->ClearEventListener();
    }
  }

  if (waiter->on_complete) {
    std::move(waiter->on_complete).Run();
  }
}

void AbpController::CheckActionCompleteConditions(const std::string& tab_id) {
  auto it = action_waiters_.find(tab_id);
  if (it == action_waiters_.end()) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.get();

  if (!waiter->IsComplete()) {
    return;
  }

  // All conditions met!
  std::unique_ptr<ActionCompleteWaiter> completed_waiter = std::move(it->second);
  action_waiters_.erase(it);

  // Clear event listener
  content::WebContents* wc = FindWebContents(tab_id);
  if (wc) {
    AbpCdpClient* client = GetOrCreateCdpClient(wc);
    if (client) {
      client->ClearEventListener();
    }
  }

  LOG(INFO) << "ABP: action_complete conditions met for tab " << tab_id;

  if (completed_waiter->on_complete) {
    std::move(completed_waiter->on_complete).Run();
  }
}

int64_t AbpController::GetVirtualTimeMs(const std::string& tab_id) {
  auto it = execution_states_.find(tab_id);
  if (it != execution_states_.end() && it->second.virtual_time_enabled) {
    return static_cast<int64_t>(it->second.virtual_time_base_ticks_ms);
  }
  return base::Time::Now().InMillisecondsSinceUnixEpoch();
}

void AbpController::OnFileChooserOpened(const std::string& chooser_id,
                                        const std::string& tab_id,
                                        base::Value::Dict info) {
  pending_file_choosers_[chooser_id] = std::move(info);
  LOG(INFO) << "ABP: File chooser opened with ID " << chooser_id
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

  client->SendCommand(
      "Runtime.evaluate", std::move(eval_params),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value::Dict)> cb, bool success,
             const std::string& result) {
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

            std::move(cb).Run(std::move(scroll_info));
          },
          std::move(callback)));
}

void AbpController::CaptureScreenshotBase64(
    const std::string& tab_id,
    base::OnceCallback<void(std::string base64, int width, int height)> callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run(std::string(), 0, 0);
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    std::move(callback).Run(std::string(), 0, 0);
    return;
  }

  // Get the view size
  gfx::Size view_size = view->GetViewBounds().size();

  // Use CopyFromSurface to capture the screen
  view->CopyFromSurface(
      gfx::Rect(),       // Empty rect = entire surface
      gfx::Size(),       // Empty size = native size
      base::TimeDelta(), // No timeout
      base::BindOnce(
          [](base::OnceCallback<void(std::string, int, int)> cb, gfx::Size size,
             const content::CopyFromSurfaceResult& result) {
            // Check if the copy failed
            if (!result.has_value()) {
              std::move(cb).Run(std::string(), 0, 0);
              return;
            }

            const SkBitmap& bitmap = result.value().bitmap;
            if (bitmap.drawsNothing()) {
              std::move(cb).Run(std::string(), 0, 0);
              return;
            }

            // Encode as WebP
            std::optional<std::vector<uint8_t>> encoded =
                gfx::WebpCodec::Encode(bitmap, 80);

            if (!encoded || encoded->empty()) {
              std::move(cb).Run(std::string(), 0, 0);
              return;
            }

            // Base64 encode
            std::string base64 = base::Base64Encode(*encoded);

            std::move(cb).Run(std::move(base64), bitmap.width(), bitmap.height());
          },
          std::move(callback), view_size));
}

// Dialog endpoints

void AbpController::GetDialog(const std::string& tab_id,
                              ResponseCallback callback) {
  auto it = pending_dialogs_.find(tab_id);

  base::Value::Dict response;
  if (it != pending_dialogs_.end()) {
    response.Set("present", true);
    response.Set("dialog_type", it->second.dialog_type);
    response.Set("message", it->second.message);
    if (!it->second.default_prompt.empty()) {
      response.Set("default_prompt", it->second.default_prompt);
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

  auto it = pending_dialogs_.find(tab_id);
  if (it == pending_dialogs_.end()) {
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
            weak_this->pending_dialogs_.erase(tab_id);

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

  auto it = pending_dialogs_.find(tab_id);
  if (it == pending_dialogs_.end()) {
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
            weak_this->pending_dialogs_.erase(tab_id);

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
  pending_dialogs_[tab_id] = std::move(dialog);
  LOG(INFO) << "ABP: Dialog opened in tab " << tab_id << " type=" << dialog_type;
}

void AbpController::OnDialogClosed(const std::string& tab_id) {
  pending_dialogs_.erase(tab_id);
  LOG(INFO) << "ABP: Dialog closed in tab " << tab_id;
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
    base::Value::Dict cdp_params;
    cdp_params.Set("action", "cancel");

    std::string tab_id_copy = *tab_id;
    client->SendCommand(
        "Page.handleFileChooser", std::move(cdp_params),
        base::BindOnce(
            [](base::WeakPtr<AbpController> weak_this, std::string chooser_id,
               ResponseCallback cb, bool success, const std::string& result) {
              if (!weak_this) {
                std::move(cb).Run(500, "application/json",
                                 R"({"error":"Controller destroyed"})");
                return;
              }
              // Remove from pending
              weak_this->pending_file_choosers_.erase(chooser_id);

              if (!success) {
                weak_this->SendError(500, "Failed to cancel file chooser",
                                     std::move(cb));
                return;
              }

              base::Value::Dict response;
              response.Set("success", true);
              response.Set("cancelled", true);
              weak_this->SendJson(200, base::Value(std::move(response)),
                                 std::move(cb));
            },
            weak_factory_.GetWeakPtr(), chooser_id, std::move(callback)));
    return;
  }

  // Get files to provide
  const base::Value::List* files = params.FindList("files");
  const std::string* save_path = params.FindString("path");

  if (!files && !save_path) {
    SendError(400, "Must provide 'files' array or 'path' for save dialog",
              std::move(callback));
    return;
  }

  base::Value::Dict cdp_params;
  cdp_params.Set("action", "accept");

  base::Value::List file_list;
  if (files) {
    for (const auto& file : *files) {
      if (file.is_string()) {
        file_list.Append(file.GetString());
      }
    }
  } else if (save_path) {
    file_list.Append(*save_path);
  }
  cdp_params.Set("files", std::move(file_list));

  client->SendCommand(
      "Page.handleFileChooser", std::move(cdp_params),
      base::BindOnce(
          [](base::WeakPtr<AbpController> weak_this, std::string chooser_id,
             ResponseCallback cb, bool success, const std::string& result) {
            if (!weak_this) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"Controller destroyed"})");
              return;
            }
            // Remove from pending
            weak_this->pending_file_choosers_.erase(chooser_id);

            if (!success) {
              weak_this->SendError(500, "Failed to handle file chooser",
                                   std::move(cb));
              return;
            }

            base::Value::Dict response;
            response.Set("success", true);
            weak_this->SendJson(200, base::Value(std::move(response)),
                               std::move(cb));
          },
          weak_factory_.GetWeakPtr(), chooser_id, std::move(callback)));
}

// Binary screenshot endpoint (GET)

void AbpController::BinaryScreenshot(const std::string& tab_id,
                                     const std::string& query,
                                     ResponseCallback callback) {
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

  // Parse query params for markup option
  // Format: ?markup=interactive or ?markup=none
  std::string markup = "none";
  if (!query.empty()) {
    size_t pos = query.find("markup=");
    if (pos != std::string::npos) {
      size_t start = pos + 7;
      size_t end = query.find('&', start);
      markup = query.substr(start, end == std::string::npos ? end : end - start);
    }
  }

  // For binary output, we use CopyFromSurface directly
  // TODO: Add markup support later if needed
  view->CopyFromSurface(
      gfx::Rect(),       // Empty rect = entire surface
      gfx::Size(),       // Empty size = native size
      base::TimeDelta(), // No timeout
      base::BindOnce(
          [](ResponseCallback cb,
             const content::CopyFromSurfaceResult& result) {
            // Check if the copy failed
            if (!result.has_value()) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"Screenshot capture failed"})");
              return;
            }

            const SkBitmap& bitmap = result.value().bitmap;
            if (bitmap.drawsNothing()) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"Empty screenshot"})");
              return;
            }

            // Encode as WebP
            std::optional<std::vector<uint8_t>> encoded =
                gfx::WebpCodec::Encode(bitmap, 80);

            if (!encoded || encoded->empty()) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"WebP encoding failed"})");
              return;
            }

            // Return raw binary WebP data
            std::string binary_data(encoded->begin(), encoded->end());
            std::move(cb).Run(200, "image/webp", std::move(binary_data));
          },
          std::move(callback)));
}

// Browser shutdown endpoint

void AbpController::ShutdownBrowser(const base::Value::Dict& params,
                                    ResponseCallback callback) {
  // Get optional timeout
  int timeout_ms = params.FindInt("timeout_ms").value_or(5000);

  LOG(INFO) << "ABP: Browser shutdown requested with timeout " << timeout_ms << "ms";

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

}  // namespace abp
