#ifndef CHROME_BROWSER_ABP_ABP_MCP_HANDLER_H_
#define CHROME_BROWSER_ABP_ABP_MCP_HANDLER_H_

#include <map>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"

namespace abp {

class AbpController;

// Response callback for MCP requests
// status: HTTP status code
// content_type: "application/json" or "text/event-stream"
// body: Response body
using McpResponseCallback = base::OnceCallback<void(int status,
                                                     const std::string& content_type,
                                                     std::string body)>;

// MCP session state
struct McpSession {
  McpSession();
  ~McpSession();
  McpSession(const McpSession&) = delete;
  McpSession& operator=(const McpSession&) = delete;

  std::string id;
  std::string client_name;
  std::string client_version;
  int64_t created_at_ms = 0;
  int64_t last_activity_ms = 0;
  bool initialized = false;
};

// Handles MCP Streamable HTTP protocol on /mcp endpoint.
// Implements JSON-RPC 2.0 over HTTP with optional SSE streaming.
//
// Protocol version: 2025-03-26
// Spec: https://modelcontextprotocol.io/specification/2025-06-18/basic/transports
class AbpMcpHandler {
 public:
  explicit AbpMcpHandler(AbpController* controller);
  ~AbpMcpHandler();

  AbpMcpHandler(const AbpMcpHandler&) = delete;
  AbpMcpHandler& operator=(const AbpMcpHandler&) = delete;

  // Handle incoming MCP request
  // method: HTTP method (POST, GET, DELETE)
  // headers: HTTP headers (for session ID, auth, etc.)
  // body: Request body (JSON-RPC message)
  void HandleRequest(const std::string& method,
                     const std::map<std::string, std::string>& headers,
                     const std::string& body,
                     McpResponseCallback callback);

 private:
  // JSON-RPC method handlers
  void HandleInitialize(const base::Value::Dict& params,
                        int request_id,
                        McpResponseCallback callback);
  void HandleToolsList(int request_id, McpResponseCallback callback);
  void HandleToolsCall(const base::Value::Dict& params,
                       int request_id,
                       McpResponseCallback callback);

  // Tool implementations (delegate to AbpController)
  void CallBrowserGetStatus(const base::Value::Dict& args,
                            int request_id,
                            McpResponseCallback callback);
  void CallBrowserListTabs(const base::Value::Dict& args,
                           int request_id,
                           McpResponseCallback callback);
  void CallBrowserNewTab(const base::Value::Dict& args,
                         int request_id,
                         McpResponseCallback callback);
  void CallBrowserCloseTab(const base::Value::Dict& args,
                           int request_id,
                           McpResponseCallback callback);
  void CallBrowserGetTabInfo(const base::Value::Dict& args,
                             int request_id,
                             McpResponseCallback callback);
  void CallBrowserNavigate(const base::Value::Dict& args,
                           int request_id,
                           McpResponseCallback callback);
  void CallBrowserGoBack(const base::Value::Dict& args,
                         int request_id,
                         McpResponseCallback callback);
  void CallBrowserGoForward(const base::Value::Dict& args,
                            int request_id,
                            McpResponseCallback callback);
  void CallBrowserReload(const base::Value::Dict& args,
                         int request_id,
                         McpResponseCallback callback);
  void CallBrowserClick(const base::Value::Dict& args,
                        int request_id,
                        McpResponseCallback callback);
  void CallBrowserType(const base::Value::Dict& args,
                       int request_id,
                       McpResponseCallback callback);
  void CallBrowserScreenshot(const base::Value::Dict& args,
                             int request_id,
                             McpResponseCallback callback);
  void CallBrowserExecuteJavascript(const base::Value::Dict& args,
                                    int request_id,
                                    McpResponseCallback callback);
  void CallBrowserKeyboardPress(const base::Value::Dict& args,
                                int request_id,
                                McpResponseCallback callback);
  void CallBrowserScroll(const base::Value::Dict& args,
                         int request_id,
                         McpResponseCallback callback);
  void CallBrowserMouseMove(const base::Value::Dict& args,
                            int request_id,
                            McpResponseCallback callback);
  void CallBrowserActivateTab(const base::Value::Dict& args,
                              int request_id,
                              McpResponseCallback callback);
  void CallBrowserStopLoading(const base::Value::Dict& args,
                              int request_id,
                              McpResponseCallback callback);
  void CallBrowserGetDialog(const base::Value::Dict& args,
                            int request_id,
                            McpResponseCallback callback);
  void CallBrowserAcceptDialog(const base::Value::Dict& args,
                               int request_id,
                               McpResponseCallback callback);
  void CallBrowserDismissDialog(const base::Value::Dict& args,
                                int request_id,
                                McpResponseCallback callback);
  void CallBrowserListDownloads(const base::Value::Dict& args,
                                int request_id,
                                McpResponseCallback callback);
  void CallBrowserGetDownload(const base::Value::Dict& args,
                              int request_id,
                              McpResponseCallback callback);
  void CallBrowserCancelDownload(const base::Value::Dict& args,
                                 int request_id,
                                 McpResponseCallback callback);
  void CallBrowserProvideFiles(const base::Value::Dict& args,
                               int request_id,
                               McpResponseCallback callback);
  void CallBrowserKeyboardDown(const base::Value::Dict& args,
                               int request_id,
                               McpResponseCallback callback);
  void CallBrowserKeyboardUp(const base::Value::Dict& args,
                             int request_id,
                             McpResponseCallback callback);
  void CallBrowserGetExecutionState(const base::Value::Dict& args,
                                    int request_id,
                                    McpResponseCallback callback);
  void CallBrowserSetExecutionState(const base::Value::Dict& args,
                                    int request_id,
                                    McpResponseCallback callback);
  void CallBrowserShutdown(const base::Value::Dict& args,
                           int request_id,
                           McpResponseCallback callback);

  // Response helpers
  void SendJsonRpcResult(int request_id,
                         base::Value result,
                         McpResponseCallback callback);
  void SendJsonRpcError(int request_id,
                        int error_code,
                        const std::string& message,
                        McpResponseCallback callback);
  void SendAccepted(McpResponseCallback callback);

  // Callback adapter: converts AbpController response to MCP tool result
  void OnControllerResponse(int request_id,
                            McpResponseCallback callback,
                            int status,
                            const std::string& content_type,
                            std::string body);

  // Session management
  std::string CreateSession(const std::string& client_name,
                            const std::string& client_version);
  McpSession* GetSession(const std::string& session_id);
  void CleanupExpiredSessions();

  // Controller reference (not owned)
  raw_ptr<AbpController> controller_;

  // Active sessions
  std::map<std::string, std::unique_ptr<McpSession>> sessions_;

  // Session timeout in milliseconds (30 minutes)
  static constexpr int64_t kSessionTimeoutMs = 30 * 60 * 1000;

  base::WeakPtrFactory<AbpMcpHandler> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_MCP_HANDLER_H_
