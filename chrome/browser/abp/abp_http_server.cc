// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_http_server.h"

#include <optional>

#include "build/build_config.h"

#if BUILDFLAG(IS_MAC)
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_config.h"
#include "chrome/browser/abp/abp_controller.h"
#include "chrome/browser/abp/abp_download_observer.h"
#include "chrome/browser/abp/abp_event_observer.h"
#include "chrome/browser/abp/abp_history_controller.h"
#include "chrome/browser/abp/abp_mcp_handler.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/common/content_switches.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/server/http_server_request_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace abp {

namespace {

constexpr net::NetworkTrafficAnnotationTag kAbpTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("abp_http_server", R"(
      semantics {
        sender: "Agent Browser Protocol"
        description:
          "HTTP API responses from the Agent Browser Protocol server for "
          "local AI agent browser automation."
        trigger:
          "A local client makes requests to the ABP server on localhost."
        data: "JSON API responses for browser control operations."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting:
          "This server runs automatically in ABP builds "
          "and only accepts connections from localhost."
        policy_exception_justification:
          "ABP HTTP server is always active in ABP builds."
      })");

#if BUILDFLAG(IS_LINUX)
bool HasUnsupportedRenderingFlags() {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  return command_line.HasSwitch(switches::kDisableGpu) &&
         command_line.HasSwitch(switches::kDisableSoftwareRasterizer);
}
#endif

}  // namespace

AbpHttpServer::AbpHttpServer(int port) : port_(port) {}

AbpHttpServer::~AbpHttpServer() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Stop download observer
  if (download_observer_) {
    download_observer_->Stop();
  }

  // Stop event observer
  if (event_observer_) {
    event_observer_->Stop();
  }

  // Shutdown history (flushes database)
  if (history_controller_) {
    history_controller_->Shutdown();
  }

#if BUILDFLAG(IS_MAC)
  // Release display sleep assertion
  if (display_sleep_assertion_ != kIOPMNullAssertionID) {
    IOPMAssertionRelease(display_sleep_assertion_);
    display_sleep_assertion_ = kIOPMNullAssertionID;
  }
#endif
}

void AbpHttpServer::Start() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

#if BUILDFLAG(IS_LINUX)
  if (HasUnsupportedRenderingFlags()) {
    LOG(ERROR)
        << "ABP: refusing to start with both --disable-gpu and "
           "--disable-software-rasterizer. ABP needs either hardware GPU "
           "rendering or the software rasterizer for compositor-backed "
           "screenshots. Remove --disable-software-rasterizer, or keep GPU "
           "disabled and use the bundled software fallback instead.";
    chrome::AttemptExit();
    return;
  }
#endif

  // Load configuration
  AbpConfig config = LoadAbpConfig();

  // Print session directory at startup
  VLOG(1) << "ABP: Session directory: " << config.session_dir.value();

  // Create history controller
  history_controller_ = std::make_unique<AbpHistoryController>(config);
  history_controller_->Initialize();

  // Create controller and connect to history
  controller_ = std::make_unique<AbpController>();
  controller_->SetTimingConfig(config.timing);
  controller_->SetHistoryController(history_controller_.get());
  controller_->SetSessionDir(config.session_dir);

  // Create event observer for history
  if (history_controller_->IsEnabled()) {
    event_observer_ =
        std::make_unique<AbpEventObserver>(history_controller_.get());
    event_observer_->Start();
    controller_->SetEventObserver(event_observer_.get());
  }

  // Create download observer
  download_observer_ = std::make_unique<AbpDownloadObserver>(controller_.get());
  download_observer_->Start();
  controller_->SetDownloadObserver(download_observer_.get());

  // Create MCP handler
  mcp_handler_ = std::make_unique<AbpMcpHandler>(controller_.get());

#if BUILDFLAG(IS_MAC)
  // Wake the display and prevent it from sleeping while ABP is active.
  // The renderer's compositor stops producing frames when the display is
  // asleep, which causes all screenshot methods (CopyFromSurface,
  // ForceRedraw, GrabWindowSnapshot) to hang or return empty results.

  // First, wake the display if it's currently asleep
  IOPMAssertionID wake_assertion = kIOPMNullAssertionID;
  IOPMAssertionDeclareUserActivity(
      CFSTR("Agent Browser Protocol waking display"),
      kIOPMUserActiveLocal, &wake_assertion);

  // Then prevent future display sleep
  CFStringRef reason = CFSTR("Agent Browser Protocol active session");
  IOReturn result = IOPMAssertionCreateWithName(
      kIOPMAssertionTypePreventUserIdleDisplaySleep,
      kIOPMAssertionLevelOn, reason, &display_sleep_assertion_);
  if (result == kIOReturnSuccess) {
    VLOG(1) << "ABP: Display wake + sleep prevention enabled";
  } else {
    LOG(WARNING) << "ABP: Failed to prevent display sleep: " << result;
    display_sleep_assertion_ = kIOPMNullAssertionID;
  }
#endif

  // Safe to use Unretained because AbpHttpServer is a singleton that lives
  // for the lifetime of the browser process.
  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::StartOnIO, base::Unretained(this)));

  // ABP always starts in agent mode (system inputs blocked),
  // poll for browser readiness and center the mouse cursor when ready
  VLOG(1) << "ABP: System inputs blocked, will center mouse when ready";
  // Start polling after a short initial delay
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::PollForReadyAndCenterCursor,
                     base::Unretained(this)),
      base::Milliseconds(100));
}

void AbpHttpServer::PollForReadyAndCenterCursor() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (cursor_centered_) {
    return;  // Already centered
  }

  if (controller_->IsBrowserReady()) {
    VLOG(1) << "ABP: Browser ready, centering cursor";
    cursor_centered_ = true;
    std::string tab_id = controller_->GetActiveTabId();
    if (!tab_id.empty()) {
      controller_->CenterCursorInTab(tab_id, base::DoNothing());
    }
  } else {
    // Not ready yet, poll again
    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpHttpServer::PollForReadyAndCenterCursor,
                       base::Unretained(this)),
        base::Milliseconds(100));
  }
}

void AbpHttpServer::StartOnIO() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);

  auto socket =
      std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());

  net::IPEndPoint endpoint(net::IPAddress::IPv4Localhost(), port_);
  int rv = socket->Listen(endpoint, 5 /* backlog */, std::nullopt);
  if (rv != net::OK) {
    LOG(ERROR) << "ABP: Failed to listen on port " << port_ << ": "
               << net::ErrorToString(rv);
    return;
  }

  server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
  VLOG(1) << "ABP: HTTP server started on http://localhost:" << port_;
}

void AbpHttpServer::OnConnect(int connection_id) {
  // Increase write buffer from default 1MB to 10MB. Network API responses
  // with bodies can exceed 1MB, causing writes to fail and connections to hang.
  server_->SetSendBufferSize(connection_id, 10 * 1024 * 1024);
}

void AbpHttpServer::OnHttpRequest(int connection_id,
                                  const net::HttpServerRequestInfo& info) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);

  // Copy headers for MCP handling
  std::map<std::string, std::string> headers;
  for (const auto& pair : info.headers) {
    // Convert header names to lowercase for consistent lookup
    std::string lower_name = base::ToLowerASCII(pair.first);
    headers[lower_name] = pair.second;
  }

  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::HandleRequestOnUI,
                     base::Unretained(this),
                     connection_id,
                     info.method,
                     info.path,
                     info.data,
                     std::move(headers)));
}

void AbpHttpServer::OnWebSocketRequest(int connection_id,
                                       const net::HttpServerRequestInfo& info) {
  // Not supported
  SendResponseOnIO(connection_id, 400, "text/plain", {}, "WebSocket not supported");
}

void AbpHttpServer::OnWebSocketMessage(int connection_id, std::string data) {}

void AbpHttpServer::OnClose(int connection_id) {}

void AbpHttpServer::HandleRequestOnUI(int connection_id,
                                      std::string method,
                                      std::string path,
                                      std::string body,
                                      std::map<std::string, std::string> headers) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Route MCP requests to MCP handler
  // Path format: /mcp
  std::string clean_path = path;
  size_t query_pos = path.find('?');
  if (query_pos != std::string::npos) {
    clean_path = path.substr(0, query_pos);
  }

  if (clean_path == "/mcp") {
    // MCP handler uses callback with headers support
    auto mcp_callback = base::BindOnce(&AbpHttpServer::OnResponseWithHeadersReady,
                                       base::Unretained(this),
                                       connection_id);
    mcp_handler_->HandleRequest(method, headers, body, std::move(mcp_callback));
    return;
  }

  // Standard callback for non-MCP requests
  auto callback = base::BindOnce(&AbpHttpServer::OnResponseReady,
                                 base::Unretained(this),
                                 connection_id);

  // Route history requests to history controller
  // Path format: /api/v1/history/...
  if (base::StartsWith(clean_path, "/api/v1/history",
                       base::CompareCase::SENSITIVE)) {
    if (history_controller_) {
      history_controller_->HandleRequest(method, path, body,
                                         std::move(callback));
    } else {
      // History disabled
      std::move(callback).Run(
          503, "application/json",
          R"({"success":false,"error":"HISTORY_DISABLED"})");
    }
    return;
  }

  controller_->HandleRequest(method, path, body, std::move(callback));
}

void AbpHttpServer::OnResponseReady(int connection_id,
                                    int status_code,
                                    const std::string& content_type,
                                    std::string body) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::SendResponseOnIO,
                     base::Unretained(this),
                     connection_id,
                     status_code,
                     content_type,
                     std::map<std::string, std::string>(),
                     std::move(body)));
}

void AbpHttpServer::OnResponseWithHeadersReady(
    int connection_id,
    int status_code,
    const std::string& content_type,
    std::map<std::string, std::string> headers,
    std::string body) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::SendResponseOnIO,
                     base::Unretained(this),
                     connection_id,
                     status_code,
                     content_type,
                     std::move(headers),
                     std::move(body)));
}

void AbpHttpServer::SendResponseOnIO(
    int connection_id,
    int status_code,
    const std::string& content_type,
    const std::map<std::string, std::string>& headers,
    const std::string& body) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);

  if (!server_) {
    return;
  }

  net::HttpStatusCode status = static_cast<net::HttpStatusCode>(status_code);

  // If we have custom headers, build and send raw response
  if (!headers.empty()) {
    std::string response = "HTTP/1.1 ";
    response += base::NumberToString(status_code);
    response += " ";
    // Add reason phrase
    switch (status_code) {
      case 200: response += "OK"; break;
      case 202: response += "Accepted"; break;
      case 204: response += "No Content"; break;
      case 400: response += "Bad Request"; break;
      case 404: response += "Not Found"; break;
      case 405: response += "Method Not Allowed"; break;
      case 500: response += "Internal Server Error"; break;
      default: response += "Unknown"; break;
    }
    response += "\r\n";

    // Add content type
    response += "Content-Type: ";
    response += content_type;
    response += "\r\n";

    // Add content length
    response += "Content-Length: ";
    response += base::NumberToString(body.size());
    response += "\r\n";

    // Add custom headers
    for (const auto& header : headers) {
      response += header.first;
      response += ": ";
      response += header.second;
      response += "\r\n";
    }

    // End headers
    response += "\r\n";

    // Add body
    response += body;

    server_->SendRaw(connection_id, response, kAbpTrafficAnnotation);
  } else {
    // No custom headers, use standard Send
    server_->Send(connection_id, status, body, content_type, kAbpTrafficAnnotation);
  }
}

}  // namespace abp
