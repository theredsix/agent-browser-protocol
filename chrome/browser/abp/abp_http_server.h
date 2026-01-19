#ifndef CHROME_BROWSER_ABP_ABP_HTTP_SERVER_H_
#define CHROME_BROWSER_ABP_ABP_HTTP_SERVER_H_

#include <memory>
#include <string>

#include "net/server/http_server.h"

namespace abp {

class AbpController;
class AbpEventObserver;
class AbpHistoryController;

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

  // Poll for browser readiness and center cursor when ready
  void PollForReadyAndCenterCursor();

  const int port_;
  bool cursor_centered_ = false;  // Track whether initial cursor centering is done
  std::unique_ptr<net::HttpServer> server_;  // IO thread only
  std::unique_ptr<AbpController> controller_;  // UI thread only
  std::unique_ptr<AbpHistoryController> history_controller_;  // UI thread only
  std::unique_ptr<AbpEventObserver> event_observer_;  // UI thread only
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_HTTP_SERVER_H_
