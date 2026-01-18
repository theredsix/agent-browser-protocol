#include "chrome/browser/abp/abp_controller.h"

#include <algorithm>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_cursor_icons.h"
#include "chrome/browser/abp/abp_history_controller.h"
#include "chrome/browser/abp/abp_mouse_tracker.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/encode/SkWebpEncoder.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/cursor/mojom/cursor_type.mojom.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "ui/gfx/codec/webp_codec.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace abp {

// ActionContext implementation
ActionContext::ActionContext() = default;
ActionContext::~ActionContext() = default;
ActionContext::ActionContext(ActionContext&&) = default;
ActionContext& ActionContext::operator=(ActionContext&&) = default;

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
void ProcessAndSaveScreenshot(
    const base::FilePath& screenshot_path,
    double cursor_x,
    double cursor_y,
    ui::mojom::CursorType cursor_type,
    float device_scale_factor,
    base::OnceCallback<void(std::string path)> callback,
    SkBitmap bitmap) {
  // This runs on a background thread

  // Use the bitmap directly - it was already copied when passed here
  SkBitmap output_bitmap = std::move(bitmap);

  // Draw cursor overlay if position is valid
  if (cursor_x >= 0 && cursor_y >= 0) {
    SkCanvas canvas(output_bitmap);

    // Convert CSS pixels to device pixels for drawing
    gfx::PointF cursor_pos(
        static_cast<float>(cursor_x * device_scale_factor),
        static_cast<float>(cursor_y * device_scale_factor));

    // Draw the cursor icon
    DrawCursorIcon(&canvas, cursor_type, cursor_pos, device_scale_factor);

    LOG(INFO) << "ABP: Drew cursor at device pixels ("
              << cursor_pos.x() << "," << cursor_pos.y() << ")";
  }

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
    // This is an event notification, ignore for now
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

AbpController::AbpController() = default;
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

  // Send mouseMoved event to center
  base::Value::Dict cdp_params;
  cdp_params.Set("type", "mouseMoved");
  cdp_params.Set("x", center_x);
  cdp_params.Set("y", center_y);

  client->SendCommand(
      "Input.dispatchMouseEvent", cdp_params,
      base::BindOnce([](bool success, const std::string& result) {
        if (success) {
          LOG(INFO) << "ABP: Mouse centered successfully";
        } else {
          LOG(WARNING) << "ABP: Failed to center mouse: " << result;
        }
      }));
}

void AbpController::CaptureScreenshotForHistory(
    const std::string& tab_id,
    int64_t timestamp,
    bool is_before,
    base::OnceCallback<void(std::string path)> callback,
    double cursor_x,
    double cursor_y) {
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
  CaptureScreenshotDirect(wc, screenshot_path, cursor_x, cursor_y,
                          std::move(callback));
}

void AbpController::CaptureScreenshotDirect(
    content::WebContents* web_contents,
    const base::FilePath& screenshot_path,
    double cursor_x,
    double cursor_y,
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

  // If cursor position not provided, get it from the mouse tracker
  double effective_cursor_x = cursor_x;
  double effective_cursor_y = cursor_y;

  if (cursor_x < 0 || cursor_y < 0) {
    gfx::PointF tracked_pos =
        GetMouseTracker()->GetMousePositionInView(web_contents);
    if (tracked_pos.x() >= 0 && tracked_pos.y() >= 0) {
      effective_cursor_x = tracked_pos.x();
      effective_cursor_y = tracked_pos.y();
      LOG(INFO) << "ABP: Using tracked mouse position: ("
                << effective_cursor_x << "," << effective_cursor_y << ")";
    }
  }

  // Get cursor type from the public API
  ui::mojom::CursorType cursor_type = rwhv->GetLastCursorType();
  LOG(INFO) << "ABP: Got cursor type: " << static_cast<int>(cursor_type);

  // Get device scale factor for proper DPI handling
  float device_scale_factor = rwhv->GetDeviceScaleFactor();

  LOG(INFO) << "ABP: Direct screenshot capture - cursor_type="
            << static_cast<int>(cursor_type)
            << " scale=" << device_scale_factor
            << " cursor_pos=(" << effective_cursor_x << "," << effective_cursor_y << ")";

  // Capture the surface directly with 5 second timeout
  rwhv->CopyFromSurface(
      gfx::Rect(),   // empty = full viewport
      gfx::Size(),   // empty = native resolution
      base::Seconds(5),  // timeout
      base::BindOnce(&AbpController::OnSurfaceCopied,
                     weak_factory_.GetWeakPtr(),
                     screenshot_path,
                     effective_cursor_x,
                     effective_cursor_y,
                     cursor_type,
                     device_scale_factor,
                     std::move(callback)));
}

void AbpController::OnSurfaceCopied(
    const base::FilePath& screenshot_path,
    double cursor_x,
    double cursor_y,
    ui::mojom::CursorType cursor_type,
    float device_scale_factor,
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

  LOG(INFO) << "ABP: Surface copied - bitmap size="
            << bitmap.width() << "x" << bitmap.height();

  // Make a deep copy to pass to the background thread
  SkBitmap bitmap_copy;
  bitmap_copy.allocPixels(bitmap.info());
  bitmap.readPixels(bitmap_copy.info(), bitmap_copy.getPixels(),
                    bitmap_copy.rowBytes(), 0, 0);

  // Process and save on a background thread (using free function in anon namespace)
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::TaskPriority::USER_VISIBLE, base::MayBlock()},
      base::BindOnce(&ProcessAndSaveScreenshot,
                     screenshot_path,
                     cursor_x,
                     cursor_y,
                     cursor_type,
                     device_scale_factor,
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
        Screenshot(tab_id, params, std::move(callback));
      } else if (action == "execute") {
        ExecuteScript(tab_id, params, std::move(callback));
      } else if (action == "click") {
        Click(tab_id, params, std::move(callback));
      } else if (action == "type") {
        Type(tab_id, params, std::move(callback));
      } else {
        SendError(404, "Unknown action: " + action, std::move(callback));
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
        // Remove CDP client if exists
        cdp_clients_.erase(tab_id);
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
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    // Record failed action
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "navigate", params, nullptr,
                                        false, "TAB_NOT_FOUND", "Tab not found",
                                        start_time, 0, "", "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  const std::string* url = params.FindString("url");
  if (!url) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "navigate", params, nullptr,
                                        false, "MISSING_PARAM",
                                        "Missing 'url' parameter", start_time,
                                        0, "", "");
    }
    SendError(400, "Missing 'url' parameter", std::move(callback));
    return;
  }

  GURL gurl(*url);
  if (!gurl.is_valid()) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "navigate", params, nullptr,
                                        false, "INVALID_URL", "Invalid URL",
                                        start_time, 0, "", "");
    }
    SendError(400, "Invalid URL", std::move(callback));
    return;
  }

  // Store context for recording
  auto context = std::make_unique<ActionContext>();
  context->tab_id = tab_id;
  context->action_type = "navigate";
  context->params = params.Clone();
  context->start_time = start_time;

  std::string url_copy = gurl.spec();

  // Take "before" screenshot first, then proceed with navigate
  CaptureScreenshotForHistory(
      tab_id, start_time, true,
      base::BindOnce(&AbpController::OnNavigateBeforeScreenshot,
                     weak_factory_.GetWeakPtr(), tab_id, url_copy,
                     std::move(context), std::move(callback)));
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

  GURL gurl(url);
  wc->GetController().LoadURL(gurl, content::Referrer(),
                              ui::PAGE_TRANSITION_TYPED, std::string());

  // Take "after" screenshot, then record and respond
  CaptureScreenshotForHistory(
      tab_id, context->start_time, false,
      base::BindOnce(&AbpController::OnNavigateAfterScreenshot,
                     weak_factory_.GetWeakPtr(), url, std::move(context),
                     std::move(callback)));
}

void AbpController::OnNavigateAfterScreenshot(
    const std::string& url,
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  base::Value::Dict result;
  result.Set("status", "navigating");
  result.Set("url", url);

  // Record successful action with both screenshot paths
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
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  base::Value::Dict params;

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "reload", params, nullptr,
                                        false, "TAB_NOT_FOUND", "Tab not found",
                                        start_time, 0, "", "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  // Store context for recording
  auto context = std::make_unique<ActionContext>();
  context->tab_id = tab_id;
  context->action_type = "reload";
  context->params = std::move(params);
  context->start_time = start_time;

  // Take "before" screenshot first, then proceed with reload
  CaptureScreenshotForHistory(
      tab_id, start_time, true,
      base::BindOnce(&AbpController::OnReloadBeforeScreenshot,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(context),
                     std::move(callback)));
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

  // Take "after" screenshot, then record and respond
  CaptureScreenshotForHistory(
      tab_id, context->start_time, false,
      base::BindOnce(&AbpController::OnReloadAfterScreenshot,
                     weak_factory_.GetWeakPtr(), std::move(context),
                     std::move(callback)));
}

void AbpController::OnReloadAfterScreenshot(
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  base::Value::Dict result;
  result.Set("status", "reloading");

  // Record successful action with both screenshot paths
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
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  base::Value::Dict params;

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "back", params, nullptr, false,
                                        "TAB_NOT_FOUND", "Tab not found",
                                        start_time, 0, "", "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  if (!wc->GetController().CanGoBack()) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "back", params, nullptr, false,
                                        "CANNOT_GO_BACK", "Cannot go back",
                                        start_time, 0, "", "");
    }
    SendError(400, "Cannot go back", std::move(callback));
    return;
  }

  // Store context for recording
  auto context = std::make_unique<ActionContext>();
  context->tab_id = tab_id;
  context->action_type = "back";
  context->params = std::move(params);
  context->start_time = start_time;

  // Take "before" screenshot first, then proceed with go back
  CaptureScreenshotForHistory(
      tab_id, start_time, true,
      base::BindOnce(&AbpController::OnGoBackBeforeScreenshot,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(context),
                     std::move(callback)));
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

  // Take "after" screenshot, then record and respond
  CaptureScreenshotForHistory(
      tab_id, context->start_time, false,
      base::BindOnce(&AbpController::OnGoBackAfterScreenshot,
                     weak_factory_.GetWeakPtr(), std::move(context),
                     std::move(callback)));
}

void AbpController::OnGoBackAfterScreenshot(
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  base::Value::Dict result;

  // Record successful action with both screenshot paths
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
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  base::Value::Dict params;

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "forward", params, nullptr,
                                        false, "TAB_NOT_FOUND", "Tab not found",
                                        start_time, 0, "", "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  if (!wc->GetController().CanGoForward()) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "forward", params, nullptr,
                                        false, "CANNOT_GO_FORWARD",
                                        "Cannot go forward", start_time, 0, "",
                                        "");
    }
    SendError(400, "Cannot go forward", std::move(callback));
    return;
  }

  // Store context for recording
  auto context = std::make_unique<ActionContext>();
  context->tab_id = tab_id;
  context->action_type = "forward";
  context->params = std::move(params);
  context->start_time = start_time;

  // Take "before" screenshot first, then proceed with go forward
  CaptureScreenshotForHistory(
      tab_id, start_time, true,
      base::BindOnce(&AbpController::OnGoForwardBeforeScreenshot,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(context),
                     std::move(callback)));
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

  // Take "after" screenshot, then record and respond
  CaptureScreenshotForHistory(
      tab_id, context->start_time, false,
      base::BindOnce(&AbpController::OnGoForwardAfterScreenshot,
                     weak_factory_.GetWeakPtr(), std::move(context),
                     std::move(callback)));
}

void AbpController::OnGoForwardAfterScreenshot(
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  base::Value::Dict result;

  // Record successful action with both screenshot paths
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

  // CDP: Page.captureScreenshot
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
  // Markup overlay already injected by the JavaScript, now take screenshot
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

  // Take screenshot
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
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "click", params, nullptr, false,
                                        "TAB_NOT_FOUND", "Tab not found",
                                        start_time, 0, "", "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  auto x = params.FindDouble("x");
  auto y = params.FindDouble("y");
  if (!x || !y) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "click", params, nullptr, false,
                                        "MISSING_PARAM",
                                        "Missing 'x' or 'y' parameter",
                                        start_time, 0, "", "");
    }
    SendError(400, "Missing 'x' or 'y' parameter", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "click", params, nullptr, false,
                                        "CDP_ERROR",
                                        "Failed to create CDP client",
                                        start_time, 0, "", "");
    }
    SendError(500, "Failed to create CDP client", std::move(callback));
    return;
  }

  // Store context for recording
  auto context = std::make_unique<ActionContext>();
  context->tab_id = tab_id;
  context->action_type = "click";
  context->params = params.Clone();
  context->start_time = start_time;

  // Take "before" screenshot first, then proceed with click
  // Pass click coordinates so cursor is positioned at target
  CaptureScreenshotForHistory(
      tab_id, start_time, true,
      base::BindOnce(&AbpController::OnClickBeforeScreenshot,
                     weak_factory_.GetWeakPtr(), tab_id, *x, *y,
                     std::move(context), std::move(callback)),
      *x, *y);
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

  // Take "after" screenshot, then record and respond
  if (context) {
    // Get click coordinates from params for cursor positioning
    double cursor_x = context->params.FindDouble("x").value_or(-1);
    double cursor_y = context->params.FindDouble("y").value_or(-1);
    CaptureScreenshotForHistory(
        context->tab_id, context->start_time, false,
        base::BindOnce(&AbpController::OnClickAfterScreenshot,
                       weak_factory_.GetWeakPtr(), std::move(context),
                       std::move(callback)),
        cursor_x, cursor_y);
  } else {
    base::Value::Dict response;
    response.Set("status", "clicked");
    SendJson(200, base::Value(std::move(response)), std::move(callback));
  }
}

void AbpController::OnClickAfterScreenshot(
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  base::Value::Dict response;
  response.Set("status", "clicked");

  // Record successful action with both screenshot paths
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
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "type", params, nullptr, false,
                                        "TAB_NOT_FOUND", "Tab not found",
                                        start_time, 0, "", "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  const std::string* text = params.FindString("text");
  if (!text) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "type", params, nullptr, false,
                                        "MISSING_PARAM",
                                        "Missing 'text' parameter", start_time,
                                        0, "", "");
    }
    SendError(400, "Missing 'text' parameter", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "type", params, nullptr, false,
                                        "CDP_ERROR",
                                        "Failed to create CDP client",
                                        start_time, 0, "", "");
    }
    SendError(500, "Failed to create CDP client", std::move(callback));
    return;
  }

  // Store context for recording
  auto context = std::make_unique<ActionContext>();
  context->tab_id = tab_id;
  context->action_type = "type";
  context->params = params.Clone();
  context->start_time = start_time;

  std::string text_copy = *text;

  // Take "before" screenshot first, then proceed with type
  CaptureScreenshotForHistory(
      tab_id, start_time, true,
      base::BindOnce(&AbpController::OnTypeBeforeScreenshot,
                     weak_factory_.GetWeakPtr(), tab_id, text_copy,
                     std::move(context), std::move(callback)));
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

  // Take "after" screenshot, then record and respond
  if (context) {
    CaptureScreenshotForHistory(
        context->tab_id, context->start_time, false,
        base::BindOnce(&AbpController::OnTypeAfterScreenshot,
                       weak_factory_.GetWeakPtr(), std::move(context),
                       std::move(callback)));
  } else {
    base::Value::Dict response;
    response.Set("status", "typed");
    SendJson(200, base::Value(std::move(response)), std::move(callback));
  }
}

void AbpController::OnTypeAfterScreenshot(
    std::unique_ptr<ActionContext> context,
    ResponseCallback callback,
    std::string screenshot_after_path) {
  base::Value::Dict response;
  response.Set("status", "typed");

  // Record successful action with both screenshot paths
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
  std::move(callback).Run(status, std::move(json));
}

}  // namespace abp
