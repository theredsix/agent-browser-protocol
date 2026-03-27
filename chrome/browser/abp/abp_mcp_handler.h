// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_MCP_HANDLER_H_
#define CHROME_BROWSER_ABP_ABP_MCP_HANDLER_H_

#include <map>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/abp/abp_types.h"

namespace abp {

class AbpController;

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
                     ResponseWithHeadersCallback callback);

 private:
  // JSON-RPC method handlers
  void HandleInitialize(const base::Value::Dict& params,
                        base::Value request_id,
                        ResponseWithHeadersCallback callback);
  void HandleToolsList(base::Value request_id, ResponseWithHeadersCallback callback);
  void HandleToolsCall(const base::Value::Dict& params,
                       base::Value request_id,
                       ResponseWithHeadersCallback callback);
  void HandleResourcesList(base::Value request_id,
                           ResponseWithHeadersCallback callback);
  void HandleResourcesRead(const base::Value::Dict& params,
                           base::Value request_id,
                           ResponseWithHeadersCallback callback);

  // Tool implementations (delegate to AbpController)
  void CallBrowserAction(const base::Value::Dict& args,
                         base::Value request_id,
                         ResponseWithHeadersCallback callback);
  void CallBrowserScroll(const base::Value::Dict& args,
                         base::Value request_id,
                         ResponseWithHeadersCallback callback);
  void CallBrowserNavigate(const base::Value::Dict& args,
                           base::Value request_id,
                           ResponseWithHeadersCallback callback);
  void CallBrowserScreenshot(const base::Value::Dict& args,
                             base::Value request_id,
                             ResponseWithHeadersCallback callback);
  void CallBrowserTabs(const base::Value::Dict& args,
                       base::Value request_id,
                       ResponseWithHeadersCallback callback);
  void CallBrowserJavascript(const base::Value::Dict& args,
                             base::Value request_id,
                             ResponseWithHeadersCallback callback);
  void CallBrowserText(const base::Value::Dict& args,
                       base::Value request_id,
                       ResponseWithHeadersCallback callback);
  void CallBrowserDialog(const base::Value::Dict& args,
                         base::Value request_id,
                         ResponseWithHeadersCallback callback);
  void CallBrowserDownloads(const base::Value::Dict& args,
                            base::Value request_id,
                            ResponseWithHeadersCallback callback);
  void CallBrowserFiles(const base::Value::Dict& args,
                        base::Value request_id,
                        ResponseWithHeadersCallback callback);
  void CallBrowserSelectPicker(const base::Value::Dict& args,
                               base::Value request_id,
                               ResponseWithHeadersCallback callback);
  void CallBrowserGetStatus(const base::Value::Dict& args,
                            base::Value request_id,
                            ResponseWithHeadersCallback callback);
  void CallBrowserShutdown(const base::Value::Dict& args,
                           base::Value request_id,
                           ResponseWithHeadersCallback callback);
  void CallBrowserSlider(const base::Value::Dict& args,
                         base::Value request_id,
                         ResponseWithHeadersCallback callback);
  void CallBrowserClearText(const base::Value::Dict& args,
                            base::Value request_id,
                            ResponseWithHeadersCallback callback);
  void CallBrowserWait(const base::Value::Dict& args,
                       base::Value request_id,
                       ResponseWithHeadersCallback callback);
  void CallRespondToPermission(const base::Value::Dict& args,
                               base::Value request_id,
                               ResponseWithHeadersCallback callback);
  void CallBrowserNetwork(const base::Value::Dict& args,
                          base::Value request_id,
                          ResponseWithHeadersCallback callback);
  void CallBrowserCurl(const base::Value::Dict& args,
                       base::Value request_id,
                       ResponseWithHeadersCallback callback);
  void CallBrowserConsole(const base::Value::Dict& args,
                          base::Value request_id,
                          ResponseWithHeadersCallback callback);
  void CallCdpMode(const base::Value::Dict& args,
                   base::Value request_id,
                   ResponseWithHeadersCallback callback);

  // Callback for browser_curl binary (image) responses.
  void OnCurlControllerResponse(base::Value request_id,
                                ResponseWithHeadersCallback callback,
                                int status,
                                const std::string& content_type,
                                std::string body);

  // Resolve tab_id from args, falling back to active tab
  std::string ResolveTabId(const base::Value::Dict& args);

  // Response helpers
  void SendJsonRpcResult(base::Value request_id,
                         base::Value result,
                         ResponseWithHeadersCallback callback);
  void SendJsonRpcError(base::Value request_id,
                        int error_code,
                        const std::string& message,
                        ResponseWithHeadersCallback callback);
  void SendAccepted(ResponseWithHeadersCallback callback);

  // Callback adapter: converts AbpController response to MCP tool result
  void OnControllerResponse(base::Value request_id,
                            ResponseWithHeadersCallback callback,
                            int status,
                            const std::string& content_type,
                            std::string body);

  // Callback for binary REST responses (download content).
  // Base64-encodes the binary data and wraps as BlobResourceContents.
  void OnBinaryControllerResponse(base::Value request_id,
                                  std::string download_id,
                                  std::string filename,
                                  ResponseWithHeadersCallback callback,
                                  int status,
                                  const std::string& content_type,
                                  std::string body);

  // Controller reference (not owned)
  raw_ptr<AbpController> controller_;

  base::WeakPtrFactory<AbpMcpHandler> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_MCP_HANDLER_H_
