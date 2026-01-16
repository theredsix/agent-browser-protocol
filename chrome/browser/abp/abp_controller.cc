#include "chrome/browser/abp/abp_controller.h"

#include <algorithm>

#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"

namespace abp {

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
  // Get the first available browser
  Browser* browser = nullptr;
  const BrowserList* browser_list = BrowserList::GetInstance();
  auto it = browser_list->begin();
  if (it != browser_list->end()) {
    browser = *it;
  }
  if (!browser) {
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
    SendJson(201, base::Value(std::move(tab)), std::move(callback));
  } else {
    SendError(500, "Failed to create tab", std::move(callback));
  }
}

void AbpController::CloseTab(const std::string& tab_id,
                             ResponseCallback callback) {
  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      if (host->GetId() == tab_id) {
        // Remove CDP client if exists
        cdp_clients_.erase(tab_id);
        tab_strip->CloseWebContentsAt(i, TabCloseTypes::CLOSE_USER_GESTURE);
        SendJson(200, base::Value(base::Value::Dict()), std::move(callback));
        return;
      }
    }
  }

  SendError(404, "Tab not found", std::move(callback));
}

void AbpController::Navigate(const std::string& tab_id,
                             const base::Value::Dict& params,
                             ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

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

  wc->GetController().LoadURL(gurl, content::Referrer(),
                              ui::PAGE_TRANSITION_TYPED, std::string());

  base::Value::Dict result;
  result.Set("status", "navigating");
  result.Set("url", gurl.spec());
  SendJson(200, base::Value(std::move(result)), std::move(callback));
}

void AbpController::Reload(const std::string& tab_id,
                           ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  wc->GetController().Reload(content::ReloadType::NORMAL, false);

  base::Value::Dict result;
  result.Set("status", "reloading");
  SendJson(200, base::Value(std::move(result)), std::move(callback));
}

void AbpController::GoBack(const std::string& tab_id,
                           ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  if (wc->GetController().CanGoBack()) {
    wc->GetController().GoBack();
    SendJson(200, base::Value(base::Value::Dict()), std::move(callback));
  } else {
    SendError(400, "Cannot go back", std::move(callback));
  }
}

void AbpController::GoForward(const std::string& tab_id,
                              ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  if (wc->GetController().CanGoForward()) {
    wc->GetController().GoForward();
    SendJson(200, base::Value(base::Value::Dict()), std::move(callback));
  } else {
    SendError(400, "Cannot go forward", std::move(callback));
  }
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
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  auto x = params.FindDouble("x");
  auto y = params.FindDouble("y");
  if (!x || !y) {
    SendError(400, "Missing 'x' or 'y' parameter", std::move(callback));
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
  cdp_params.Set("x", *x);
  cdp_params.Set("y", *y);
  cdp_params.Set("button", "left");
  cdp_params.Set("clickCount", 1);

  client->SendCommand(
      "Input.dispatchMouseEvent", cdp_params,
      base::BindOnce(&AbpController::OnClickPressedResult,
                     weak_factory_.GetWeakPtr(), tab_id, *x, *y,
                     std::move(callback)));
}

void AbpController::OnClickPressedResult(const std::string& tab_id,
                                         double x,
                                         double y,
                                         ResponseCallback callback,
                                         bool success,
                                         const std::string& result) {
  if (!success) {
    SendError(500, result, std::move(callback));
    return;
  }

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

  base::Value::Dict release_params;
  release_params.Set("type", "mouseReleased");
  release_params.Set("x", x);
  release_params.Set("y", y);
  release_params.Set("button", "left");
  release_params.Set("clickCount", 1);

  client->SendCommand(
      "Input.dispatchMouseEvent", release_params,
      base::BindOnce(&AbpController::OnClickResult, weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

void AbpController::OnClickResult(ResponseCallback callback,
                                  bool success,
                                  const std::string& result) {
  if (!success) {
    SendError(500, result, std::move(callback));
    return;
  }

  base::Value::Dict response;
  response.Set("status", "clicked");
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::Type(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  const std::string* text = params.FindString("text");
  if (!text) {
    SendError(400, "Missing 'text' parameter", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to create CDP client", std::move(callback));
    return;
  }

  // CDP: Input.insertText - simpler than key events
  base::Value::Dict cdp_params;
  cdp_params.Set("text", *text);

  client->SendCommand(
      "Input.insertText", cdp_params,
      base::BindOnce(&AbpController::OnTypeResult, weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

void AbpController::OnTypeResult(ResponseCallback callback,
                                 bool success,
                                 const std::string& result) {
  if (!success) {
    SendError(500, result, std::move(callback));
    return;
  }

  base::Value::Dict response;
  response.Set("status", "typed");
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
