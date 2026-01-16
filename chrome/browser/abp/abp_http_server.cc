#include "chrome/browser/abp/abp_http_server.h"

#include <optional>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/abp/abp_controller.h"
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

AbpHttpServer::~AbpHttpServer() = default;

void AbpHttpServer::Start() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  controller_ = std::make_unique<AbpController>();

  // Safe to use Unretained because AbpHttpServer is a singleton that lives
  // for the lifetime of the browser process.
  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::StartOnIO, base::Unretained(this)));
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

  controller_->HandleRequest(method, path, body, std::move(callback));
}

void AbpHttpServer::OnResponseReady(int connection_id,
                                    int status_code,
                                    std::string body) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpHttpServer::SendResponseOnIO,
                     base::Unretained(this),
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
  server_->Send(connection_id, status, body, content_type, kAbpTrafficAnnotation);
}

}  // namespace abp
