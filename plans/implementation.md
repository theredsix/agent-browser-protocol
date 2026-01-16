# Agent Browser Protocol - Implementation Plan (Hobby Edition)

This document describes a minimal, buildable ABP implementation. Not designed for upstream merge - designed to actually work.

## Design Philosophy

1. **Minimal surface area**: 6 files, not 60
2. **No premature abstraction**: Direct code paths, no registries
3. **REST only**: Skip MCP until REST works end-to-end
4. **Localhost only**: No auth infrastructure needed
5. **UI thread is fine**: Parse JSON wherever, optimize later if needed

---

## Architecture

```
┌─────────────────────────────────────────────┐
│              HTTP Client (curl)              │
└─────────────────┬───────────────────────────┘
                  │ GET/POST /api/v1/*
                  ▼
┌─────────────────────────────────────────────┐
│  AbpHttpServer (IO thread)                  │
│  - net::HttpServer                          │
│  - Routes requests, sends responses         │
└─────────────────┬───────────────────────────┘
                  │ PostTask to UI thread
                  ▼
┌─────────────────────────────────────────────┐
│  AbpController (UI thread)                  │
│  - Direct access to Browser, TabStripModel  │
│  - Calls DevToolsAgentHost for CDP ops      │
└─────────────────────────────────────────────┘
```

**What's NOT here:**
- No MCP/JSON-RPC (add later if needed)
- No session management (stateless REST)
- No rate limiting (localhost only)
- No auth tokens (localhost only)
- No enterprise policy (hobby project)
- No fuzzing targets (not shipping)
- No SSE streaming (polling is fine)

---

## Directory Structure

```
chrome/browser/abp/
├── BUILD.gn
├── abp_controller.h      # All browser manipulation logic (UI thread)
├── abp_controller.cc
├── abp_http_server.h     # HTTP server plumbing (IO thread)
├── abp_http_server.cc
├── abp_switches.h        # --enable-abp, --abp-port
└── abp_switches.cc
```

---

## REST API (Simplified)

All endpoints return JSON. Errors return appropriate HTTP status codes.

### Tabs

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/v1/tabs` | List all tabs |
| GET | `/api/v1/tabs/{id}` | Get tab details |
| POST | `/api/v1/tabs` | Create new tab |
| DELETE | `/api/v1/tabs/{id}` | Close tab |

### Navigation

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/v1/tabs/{id}/navigate` | Navigate to URL |
| POST | `/api/v1/tabs/{id}/reload` | Reload page |
| POST | `/api/v1/tabs/{id}/back` | Go back |
| POST | `/api/v1/tabs/{id}/forward` | Go forward |

### Content

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/v1/tabs/{id}/screenshot` | Capture screenshot (base64 PNG) |
| POST | `/api/v1/tabs/{id}/execute` | Execute JavaScript |

### Input

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/v1/tabs/{id}/click` | Click at coordinates |
| POST | `/api/v1/tabs/{id}/type` | Type text |

---

## Implementation

### Command Line Switches

**File: `chrome/browser/abp/abp_switches.h`**

```cpp
#ifndef CHROME_BROWSER_ABP_ABP_SWITCHES_H_
#define CHROME_BROWSER_ABP_ABP_SWITCHES_H_

namespace abp::switches {

// Enable ABP HTTP server
extern const char kEnableAbp[];

// Port for HTTP server (default: 9222)
extern const char kAbpPort[];

}  // namespace abp::switches

#endif  // CHROME_BROWSER_ABP_ABP_SWITCHES_H_
```

**File: `chrome/browser/abp/abp_switches.cc`**

```cpp
#include "chrome/browser/abp/abp_switches.h"

namespace abp::switches {

const char kEnableAbp[] = "enable-abp";
const char kAbpPort[] = "abp-port";

}  // namespace abp::switches
```

### HTTP Server

**File: `chrome/browser/abp/abp_http_server.h`**

```cpp
#ifndef CHROME_BROWSER_ABP_ABP_HTTP_SERVER_H_
#define CHROME_BROWSER_ABP_ABP_HTTP_SERVER_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "net/server/http_server.h"

namespace abp {

class AbpController;

// HTTP server for ABP REST API.
// Created on UI thread, runs server callbacks on IO thread,
// dispatches to AbpController on UI thread.
class AbpHttpServer : public net::HttpServer::Delegate {
 public:
  explicit AbpHttpServer(int port);
  ~AbpHttpServer() override;

  AbpHttpServer(const AbpHttpServer&) = delete;
  AbpHttpServer& operator=(const AbpHttpServer&) = delete;

  // Start the server (call from UI thread)
  void Start();

 private:
  // net::HttpServer::Delegate (called on IO thread)
  void OnConnect(int connection_id) override;
  void OnHttpRequest(int connection_id,
                     const net::HttpServerRequestInfo& info) override;
  void OnWebSocketRequest(int connection_id,
                          const net::HttpServerRequestInfo& info) override;
  void OnWebSocketMessage(int connection_id, std::string data) override;
  void OnClose(int connection_id) override;

  // IO thread
  void StartOnIO();
  void SendResponseOnIO(int connection_id,
                        int status_code,
                        const std::string& content_type,
                        const std::string& body);

  // UI thread
  void HandleRequestOnUI(int connection_id,
                         std::string method,
                         std::string path,
                         std::string body);
  void OnResponseReady(int connection_id,
                       int status_code,
                       std::string body);

  const int port_;
  std::unique_ptr<net::HttpServer> server_;  // IO thread only
  std::unique_ptr<AbpController> controller_;  // UI thread only

  base::WeakPtrFactory<AbpHttpServer> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_HTTP_SERVER_H_
```

**File: `chrome/browser/abp/abp_http_server.cc`**

```cpp
#include "chrome/browser/abp/abp_http_server.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/abp/abp_controller.h"
#include "content/public/browser/browser_thread.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/server/http_server_request_info.h"
#include "net/socket/tcp_server_socket.h"

namespace abp {

AbpHttpServer::AbpHttpServer(int port) : port_(port) {}

AbpHttpServer::~AbpHttpServer() = default;

void AbpHttpServer::Start() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  controller_ = std::make_unique<AbpController>();

  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::StartOnIO, weak_factory_.GetWeakPtr()));
}

void AbpHttpServer::StartOnIO() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);

  auto socket =
      std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());

  net::IPEndPoint endpoint(net::IPAddress::IPv4Localhost(), port_);
  int rv = socket->Listen(endpoint, 5 /* backlog */);
  if (rv != net::OK) {
    LOG(ERROR) << "ABP: Failed to listen on port " << port_ << ": "
               << net::ErrorToString(rv);
    return;
  }

  server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
  LOG(INFO) << "ABP: HTTP server started on http://localhost:" << port_;
}

void AbpHttpServer::OnConnect(int connection_id) {}

void AbpHttpServer::OnHttpRequest(int connection_id,
                                  const net::HttpServerRequestInfo& info) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);

  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::HandleRequestOnUI,
                     weak_factory_.GetWeakPtr(),
                     connection_id,
                     info.method,
                     info.path,
                     info.data));
}

void AbpHttpServer::OnWebSocketRequest(int connection_id,
                                       const net::HttpServerRequestInfo& info) {
  // Not supported
  SendResponseOnIO(connection_id, 400, "text/plain", "WebSocket not supported");
}

void AbpHttpServer::OnWebSocketMessage(int connection_id, std::string data) {}

void AbpHttpServer::OnClose(int connection_id) {}

void AbpHttpServer::HandleRequestOnUI(int connection_id,
                                      std::string method,
                                      std::string path,
                                      std::string body) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  auto callback = base::BindOnce(&AbpHttpServer::OnResponseReady,
                                 weak_factory_.GetWeakPtr(),
                                 connection_id);

  controller_->HandleRequest(method, path, body, std::move(callback));
}

void AbpHttpServer::OnResponseReady(int connection_id,
                                    int status_code,
                                    std::string body) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::SendResponseOnIO,
                     weak_factory_.GetWeakPtr(),
                     connection_id,
                     status_code,
                     "application/json",
                     std::move(body)));
}

void AbpHttpServer::SendResponseOnIO(int connection_id,
                                     int status_code,
                                     const std::string& content_type,
                                     const std::string& body) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);

  if (!server_) {
    return;
  }

  net::HttpStatusCode status = static_cast<net::HttpStatusCode>(status_code);
  server_->Send(connection_id, status, body, content_type);
}

}  // namespace abp
```

### Controller

**File: `chrome/browser/abp/abp_controller.h`**

```cpp
#ifndef CHROME_BROWSER_ABP_ABP_CONTROLLER_H_
#define CHROME_BROWSER_ABP_ABP_CONTROLLER_H_

#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"

namespace content {
class WebContents;
}

namespace abp {

using ResponseCallback = base::OnceCallback<void(int status, std::string body)>;

// Handles ABP REST API requests on the UI thread.
// Provides direct access to browser windows and tabs.
class AbpController {
 public:
  AbpController();
  ~AbpController();

  AbpController(const AbpController&) = delete;
  AbpController& operator=(const AbpController&) = delete;

  // Route incoming HTTP request to appropriate handler
  void HandleRequest(const std::string& method,
                     const std::string& path,
                     const std::string& body,
                     ResponseCallback callback);

 private:
  // Tab operations
  void ListTabs(ResponseCallback callback);
  void GetTab(const std::string& tab_id, ResponseCallback callback);
  void CreateTab(const base::Value::Dict& params, ResponseCallback callback);
  void CloseTab(const std::string& tab_id, ResponseCallback callback);

  // Navigation
  void Navigate(const std::string& tab_id,
                const base::Value::Dict& params,
                ResponseCallback callback);
  void Reload(const std::string& tab_id, ResponseCallback callback);
  void GoBack(const std::string& tab_id, ResponseCallback callback);
  void GoForward(const std::string& tab_id, ResponseCallback callback);

  // Content
  void Screenshot(const std::string& tab_id, ResponseCallback callback);
  void ExecuteScript(const std::string& tab_id,
                     const base::Value::Dict& params,
                     ResponseCallback callback);

  // Input
  void Click(const std::string& tab_id,
             const base::Value::Dict& params,
             ResponseCallback callback);
  void Type(const std::string& tab_id,
            const base::Value::Dict& params,
            ResponseCallback callback);

  // Helpers
  content::WebContents* FindWebContents(const std::string& tab_id);
  void SendError(int status,
                 const std::string& error,
                 ResponseCallback callback);
  void SendJson(int status,
                base::Value value,
                ResponseCallback callback);

  base::WeakPtrFactory<AbpController> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_CONTROLLER_H_
```

**File: `chrome/browser/abp/abp_controller.cc`**

```cpp
#include "chrome/browser/abp/abp_controller.h"

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
    auto parsed = base::JSONReader::Read(body);
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
        Screenshot(tab_id, std::move(callback));
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
  Browser* browser = BrowserList::GetInstance()->GetLastActive();
  if (!browser) {
    SendError(500, "No active browser", std::move(callback));
    return;
  }

  const std::string* url = params.FindString("url");
  GURL gurl = url ? GURL(*url) : GURL("about:blank");

  NavigateParams nav_params(browser, gurl, ui::PAGE_TRANSITION_TYPED);
  nav_params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&nav_params);

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
                               ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  // TODO: Implement via CDP Page.captureScreenshot
  // For now, return not implemented
  SendError(501, "Screenshot not implemented yet", std::move(callback));
}

void AbpController::ExecuteScript(const std::string& tab_id,
                                  const base::Value::Dict& params,
                                  ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  // TODO: Implement via CDP Runtime.evaluate
  SendError(501, "ExecuteScript not implemented yet", std::move(callback));
}

void AbpController::Click(const std::string& tab_id,
                          const base::Value::Dict& params,
                          ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  // TODO: Implement via CDP Input.dispatchMouseEvent
  SendError(501, "Click not implemented yet", std::move(callback));
}

void AbpController::Type(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  // TODO: Implement via CDP Input.dispatchKeyEvent
  SendError(501, "Type not implemented yet", std::move(callback));
}

content::WebContents* AbpController::FindWebContents(const std::string& tab_id) {
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
```

### Build Configuration

**File: `chrome/browser/abp/BUILD.gn`**

```gn
import("//build/config/features.gni")

source_set("abp") {
  sources = [
    "abp_controller.cc",
    "abp_controller.h",
    "abp_http_server.cc",
    "abp_http_server.h",
    "abp_switches.cc",
    "abp_switches.h",
  ]

  deps = [
    "//base",
    "//chrome/browser/ui",
    "//content/public/browser",
    "//net:net",
    "//net/server:http_server",
    "//ui/base",
  ]
}
```

---

## Integration Points

### 1. Add to Chrome Build

**Edit: `chrome/browser/BUILD.gn`**

Add to the `deps` list:
```gn
"//chrome/browser/abp",
```

### 2. Start Server on Browser Launch

**Edit: `chrome/browser/chrome_browser_main.cc`** (or similar)

```cpp
#include "chrome/browser/abp/abp_http_server.h"
#include "chrome/browser/abp/abp_switches.h"

// In PostProfileInit or similar:
void StartAbpServerIfEnabled() {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(abp::switches::kEnableAbp)) {
    return;
  }

  int port = 9222;
  if (command_line->HasSwitch(abp::switches::kAbpPort)) {
    base::StringToInt(
        command_line->GetSwitchValueASCII(abp::switches::kAbpPort),
        &port);
  }

  // Store this somewhere it won't be destroyed
  static std::unique_ptr<abp::AbpHttpServer> g_abp_server;
  g_abp_server = std::make_unique<abp::AbpHttpServer>(port);
  g_abp_server->Start();
}
```

---

## Usage

### Start Chrome with ABP

```bash
./out/Default/chrome --enable-abp --abp-port=9222
```

### API Examples

```bash
# List all tabs
curl http://localhost:9222/api/v1/tabs

# Get specific tab
curl http://localhost:9222/api/v1/tabs/ABC123

# Create new tab
curl -X POST http://localhost:9222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Navigate existing tab
curl -X POST http://localhost:9222/api/v1/tabs/ABC123/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Close tab
curl -X DELETE http://localhost:9222/api/v1/tabs/ABC123

# Reload
curl -X POST http://localhost:9222/api/v1/tabs/ABC123/reload

# Go back/forward
curl -X POST http://localhost:9222/api/v1/tabs/ABC123/back
curl -X POST http://localhost:9222/api/v1/tabs/ABC123/forward
```

---

## Implementation Phases

### Phase 1: Foundation (1-2 days)
- [ ] Create the 6 source files
- [ ] Add BUILD.gn
- [ ] Hook into Chrome startup
- [ ] Verify server starts: `curl localhost:9222/api/v1/tabs`

### Phase 2: Tab Management (1-2 days)
- [ ] ListTabs - working
- [ ] GetTab - working
- [ ] CreateTab - working
- [ ] CloseTab - working
- [ ] Navigate - working
- [ ] Reload/Back/Forward - working

### Phase 3: CDP Integration (3-5 days)
- [ ] Screenshot via CDP `Page.captureScreenshot`
- [ ] ExecuteScript via CDP `Runtime.evaluate`
- [ ] Click via CDP `Input.dispatchMouseEvent`
- [ ] Type via CDP `Input.dispatchKeyEvent`

---

## CDP Integration Notes

The hard part is Phase 3. Here's how to approach it:

### Attaching to DevTools

```cpp
#include "content/public/browser/devtools_agent_host.h"

auto host = content::DevToolsAgentHost::GetOrCreateFor(web_contents);

// Create a client to receive responses
class AbpDevToolsClient : public content::DevToolsAgentHostClient {
 public:
  void DispatchProtocolMessage(DevToolsAgentHost* host,
                               base::span<const uint8_t> message) override {
    // Parse CDP response JSON
  }
  void AgentHostClosed(DevToolsAgentHost* host) override {}
};

// Attach and send commands
auto client = std::make_unique<AbpDevToolsClient>();
host->AttachClient(client.get());

// Send CDP command
std::string command = R"({"id":1,"method":"Page.captureScreenshot"})";
host->DispatchProtocolMessage(
    client.get(),
    base::as_bytes(base::make_span(command)));
```

### Reference Files

| Purpose | File |
|---------|------|
| DevToolsAgentHost API | `content/public/browser/devtools_agent_host.h` |
| Example client | `content/browser/devtools/devtools_http_handler.cc` |
| Screenshot impl | `content/browser/devtools/protocol/page_handler.cc` |
| Input impl | `content/browser/devtools/protocol/input_handler.cc` |

---

## What's Intentionally Missing

1. **MCP Protocol** - Add later if REST works
2. **Auth/Security** - Localhost only, not needed
3. **Rate Limiting** - Your machine, your problem
4. **Enterprise Policy** - Not shipping to enterprises
5. **Fuzzing** - Not going through security review
6. **Session Management** - Stateless REST is simpler
7. **SSE/Streaming** - Polling works fine
8. **UMA Metrics** - No telemetry infrastructure

---

## Success Criteria

The project is "done enough" when you can:

```bash
# 1. List tabs
curl localhost:9222/api/v1/tabs

# 2. Navigate
curl -X POST localhost:9222/api/v1/tabs/{id}/navigate \
  -d '{"url":"https://example.com"}'

# 3. Screenshot
curl localhost:9222/api/v1/tabs/{id}/screenshot > screenshot.json

# 4. Click
curl -X POST localhost:9222/api/v1/tabs/{id}/click \
  -d '{"x":100,"y":200}'

# 5. Type
curl -X POST localhost:9222/api/v1/tabs/{id}/type \
  -d '{"text":"hello world"}'
```

That's browser automation. Everything else is scope creep.
