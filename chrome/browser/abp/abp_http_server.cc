#include "chrome/browser/abp/abp_http_server.h"

#include <optional>

#include "base/command_line.h"
#include "base/functional/bind.h"
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
#include "chrome/browser/abp/abp_switches.h"
#include "content/public/browser/browser_thread.h"
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
          "This server only runs when Chrome is started with --enable-abp "
          "and only accepts connections from localhost."
        policy_exception_justification:
          "Not implemented, requires explicit opt-in via command line flag."
      })");

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
}

void AbpHttpServer::Start() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Load configuration
  AbpConfig config = LoadAbpConfig();

  // Print session directory at startup
  LOG(INFO) << "ABP: Session directory: " << config.session_dir.value();

  // Create history controller
  history_controller_ = std::make_unique<AbpHistoryController>(config);
  history_controller_->Initialize();

  // Create controller and connect to history
  controller_ = std::make_unique<AbpController>();
  controller_->SetHistoryController(history_controller_.get());

  // Create event observer for history
  if (history_controller_->IsEnabled()) {
    event_observer_ =
        std::make_unique<AbpEventObserver>(history_controller_.get());
    event_observer_->Start();
  }

  // Create download observer
  download_observer_ = std::make_unique<AbpDownloadObserver>(controller_.get());
  download_observer_->Start();
  controller_->SetDownloadObserver(download_observer_.get());

  // Safe to use Unretained because AbpHttpServer is a singleton that lives
  // for the lifetime of the browser process.
  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::StartOnIO, base::Unretained(this)));

  // When ABP is enabled and system inputs are blocked (the default),
  // poll for browser readiness and center the mouse cursor when ready
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kAllowSystemInputs)) {
    LOG(INFO) << "ABP: System inputs blocked, will center mouse when ready";
    // Start polling after a short initial delay
    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpHttpServer::PollForReadyAndCenterCursor,
                       base::Unretained(this)),
        base::Milliseconds(100));
  }
}

void AbpHttpServer::PollForReadyAndCenterCursor() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (cursor_centered_) {
    return;  // Already centered
  }

  if (controller_->IsBrowserReady()) {
    LOG(INFO) << "ABP: Browser ready, centering cursor";
    cursor_centered_ = true;
    controller_->CenterMouseInActiveTab();
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
  LOG(INFO) << "ABP: HTTP server started on http://localhost:" << port_;
}

void AbpHttpServer::OnConnect(int connection_id) {}

void AbpHttpServer::OnHttpRequest(int connection_id,
                                  const net::HttpServerRequestInfo& info) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);

  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::HandleRequestOnUI,
                     base::Unretained(this),
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
                                 base::Unretained(this),
                                 connection_id);

  // Route history requests to history controller
  // Path format: /api/v1/history/...
  std::string clean_path = path;
  size_t query_pos = path.find('?');
  if (query_pos != std::string::npos) {
    clean_path = path.substr(0, query_pos);
  }

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
  server_->Send(connection_id, status, body, content_type, kAbpTrafficAnnotation);
}

}  // namespace abp
