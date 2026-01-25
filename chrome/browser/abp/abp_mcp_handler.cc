#include "chrome/browser/abp/abp_mcp_handler.h"

#include <cinttypes>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_controller.h"

namespace abp {

namespace {

// MCP protocol version
constexpr char kProtocolVersion[] = "2025-03-26";

// JSON-RPC error codes
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
// constexpr int kInternalError = -32603;  // Reserved for future use

// Generate a random session ID
std::string GenerateSessionId() {
  uint64_t random1 = base::RandUint64();
  uint64_t random2 = base::RandUint64();
  return base::StringPrintf("%016" PRIx64 "%016" PRIx64, random1, random2);
}

// Get tool definitions for tools/list
base::Value::List GetToolDefinitions() {
  base::Value::List tools;

  // browser_get_status
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_get_status");
    tool.Set("description", "Get browser status and readiness");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    input_schema.Set("properties", base::Value::Dict());
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_list_tabs
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_list_tabs");
    tool.Set("description", "List all open browser tabs");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    input_schema.Set("properties", base::Value::Dict());
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_new_tab
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_new_tab");
    tool.Set("description", "Create a new browser tab");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict url_prop;
    url_prop.Set("type", "string");
    url_prop.Set("description", "URL to navigate to");
    props.Set("url", std::move(url_prop));
    input_schema.Set("properties", std::move(props));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_close_tab
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_close_tab");
    tool.Set("description", "Close a browser tab");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "ID of tab to close");
    props.Set("tab_id", std::move(tab_id_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_get_tab_info
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_get_tab_info");
    tool.Set("description", "Get detailed information about a tab");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "ID of tab");
    props.Set("tab_id", std::move(tab_id_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_navigate
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_navigate");
    tool.Set("description", "Navigate to a URL");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    base::Value::Dict url_prop;
    url_prop.Set("type", "string");
    url_prop.Set("description", "URL to navigate to");
    props.Set("url", std::move(url_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    required.Append("url");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_go_back
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_go_back");
    tool.Set("description", "Navigate back in history");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_go_forward
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_go_forward");
    tool.Set("description", "Navigate forward in history");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_reload
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_reload");
    tool.Set("description", "Reload the current page");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_click
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_click");
    tool.Set("description", "Click at coordinates on the page");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    base::Value::Dict x_prop;
    x_prop.Set("type", "number");
    x_prop.Set("description", "X coordinate");
    props.Set("x", std::move(x_prop));
    base::Value::Dict y_prop;
    y_prop.Set("type", "number");
    y_prop.Set("description", "Y coordinate");
    props.Set("y", std::move(y_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    required.Append("x");
    required.Append("y");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_type
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_type");
    tool.Set("description", "Type text at current focus position");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    base::Value::Dict text_prop;
    text_prop.Set("type", "string");
    text_prop.Set("description", "Text to type");
    props.Set("text", std::move(text_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    required.Append("text");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_screenshot
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_screenshot");
    tool.Set("description", "Take a screenshot of the page");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    base::Value::Dict markup_prop;
    markup_prop.Set("type", "string");
    markup_prop.Set("description",
                    "Element markup overlay: none, interactive, clickable, "
                    "typeable, inputs");
    props.Set("markup", std::move(markup_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_execute_javascript
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_execute_javascript");
    tool.Set("description", "Execute JavaScript in the page context");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    base::Value::Dict expr_prop;
    expr_prop.Set("type", "string");
    expr_prop.Set("description", "JavaScript expression to evaluate");
    props.Set("expression", std::move(expr_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    required.Append("expression");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  return tools;
}

}  // namespace

McpSession::McpSession() = default;
McpSession::~McpSession() = default;

AbpMcpHandler::AbpMcpHandler(AbpController* controller)
    : controller_(controller) {}

AbpMcpHandler::~AbpMcpHandler() = default;

void AbpMcpHandler::HandleRequest(
    const std::string& method,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    McpResponseCallback callback) {
  // Handle GET request (SSE stream) - not implemented yet
  if (method == "GET") {
    // For now, return method not allowed
    // TODO: Implement SSE streaming for server notifications
    std::move(callback).Run(405, "application/json",
                            R"({"error":"SSE streaming not implemented"})");
    return;
  }

  // Handle DELETE request (session termination)
  if (method == "DELETE") {
    auto it = headers.find("mcp-session-id");
    if (it != headers.end()) {
      sessions_.erase(it->second);
    }
    std::move(callback).Run(204, "application/json", "");
    return;
  }

  // Handle POST request (JSON-RPC)
  if (method != "POST") {
    std::move(callback).Run(405, "application/json",
                            R"({"error":"Method not allowed"})");
    return;
  }

  // Parse JSON-RPC request
  auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    SendJsonRpcError(0, kParseError, "Parse error", std::move(callback));
    return;
  }

  const base::Value::Dict& request = parsed->GetDict();

  // Validate JSON-RPC version
  const std::string* jsonrpc = request.FindString("jsonrpc");
  if (!jsonrpc || *jsonrpc != "2.0") {
    SendJsonRpcError(0, kInvalidRequest, "Invalid JSON-RPC version",
                     std::move(callback));
    return;
  }

  // Get request ID (may be null for notifications)
  int request_id = 0;
  if (const auto* id_value = request.Find("id")) {
    if (id_value->is_int()) {
      request_id = id_value->GetInt();
    } else if (id_value->is_string()) {
      base::StringToInt(id_value->GetString(), &request_id);
    }
  }

  // Get method
  const std::string* rpc_method = request.FindString("method");
  if (!rpc_method) {
    SendJsonRpcError(request_id, kInvalidRequest, "Missing method",
                     std::move(callback));
    return;
  }

  // Get params (optional)
  const base::Value::Dict* params = request.FindDict("params");
  base::Value::Dict empty_params;
  if (!params) {
    params = &empty_params;
  }

  // Route to appropriate handler
  if (*rpc_method == "initialize") {
    HandleInitialize(*params, request_id, std::move(callback));
  } else if (*rpc_method == "notifications/initialized") {
    // Client notification that initialization is complete
    SendAccepted(std::move(callback));
  } else if (*rpc_method == "tools/list") {
    HandleToolsList(request_id, std::move(callback));
  } else if (*rpc_method == "tools/call") {
    HandleToolsCall(*params, request_id, std::move(callback));
  } else if (*rpc_method == "ping") {
    // Simple ping/pong
    base::Value::Dict result;
    SendJsonRpcResult(request_id, base::Value(std::move(result)),
                      std::move(callback));
  } else {
    SendJsonRpcError(request_id, kMethodNotFound, "Method not found",
                     std::move(callback));
  }
}

void AbpMcpHandler::HandleInitialize(const base::Value::Dict& params,
                                     int request_id,
                                     McpResponseCallback callback) {
  // Extract client info
  std::string client_name = "unknown";
  std::string client_version = "unknown";
  if (const base::Value::Dict* client_info = params.FindDict("clientInfo")) {
    if (const std::string* name = client_info->FindString("name")) {
      client_name = *name;
    }
    if (const std::string* version = client_info->FindString("version")) {
      client_version = *version;
    }
  }

  // Create session
  std::string session_id = CreateSession(client_name, client_version);

  LOG(INFO) << "ABP MCP: New session " << session_id << " for client "
            << client_name << " v" << client_version;

  // Build response
  base::Value::Dict result;
  result.Set("protocolVersion", kProtocolVersion);

  base::Value::Dict server_info;
  server_info.Set("name", "abp-browser");
  server_info.Set("version", "1.0.0");
  result.Set("serverInfo", std::move(server_info));

  base::Value::Dict capabilities;
  base::Value::Dict tools_cap;
  capabilities.Set("tools", std::move(tools_cap));
  result.Set("capabilities", std::move(capabilities));

  // Build JSON-RPC response
  base::Value::Dict response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", request_id);
  response.Set("result", std::move(result));

  std::string response_json;
  base::JSONWriter::Write(base::Value(std::move(response)), &response_json);

  // Include session ID in custom header via special format
  // Since we can't set headers directly, we'll include it in the response
  // The HTTP server layer will extract Mcp-Session-Id from the response
  // For now, include it in the response body as _sessionId
  auto parsed = base::JSONReader::Read(response_json, base::JSON_PARSE_RFC);
  if (parsed && parsed->is_dict()) {
    base::Value::Dict& dict = parsed->GetDict();
    dict.Set("_mcpSessionId", session_id);
    base::JSONWriter::Write(*parsed, &response_json);
  }

  std::move(callback).Run(200, "application/json", response_json);
}

void AbpMcpHandler::HandleToolsList(int request_id,
                                    McpResponseCallback callback) {
  base::Value::Dict result;
  result.Set("tools", GetToolDefinitions());

  SendJsonRpcResult(request_id, base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::HandleToolsCall(const base::Value::Dict& params,
                                    int request_id,
                                    McpResponseCallback callback) {
  const std::string* name = params.FindString("name");
  if (!name) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tool name",
                     std::move(callback));
    return;
  }

  const base::Value::Dict* args = params.FindDict("arguments");
  base::Value::Dict empty_args;
  if (!args) {
    args = &empty_args;
  }

  // Route to tool implementation
  if (*name == "browser_get_status") {
    CallBrowserGetStatus(*args, request_id, std::move(callback));
  } else if (*name == "browser_list_tabs") {
    CallBrowserListTabs(*args, request_id, std::move(callback));
  } else if (*name == "browser_new_tab") {
    CallBrowserNewTab(*args, request_id, std::move(callback));
  } else if (*name == "browser_close_tab") {
    CallBrowserCloseTab(*args, request_id, std::move(callback));
  } else if (*name == "browser_get_tab_info") {
    CallBrowserGetTabInfo(*args, request_id, std::move(callback));
  } else if (*name == "browser_navigate") {
    CallBrowserNavigate(*args, request_id, std::move(callback));
  } else if (*name == "browser_go_back") {
    CallBrowserGoBack(*args, request_id, std::move(callback));
  } else if (*name == "browser_go_forward") {
    CallBrowserGoForward(*args, request_id, std::move(callback));
  } else if (*name == "browser_reload") {
    CallBrowserReload(*args, request_id, std::move(callback));
  } else if (*name == "browser_click") {
    CallBrowserClick(*args, request_id, std::move(callback));
  } else if (*name == "browser_type") {
    CallBrowserType(*args, request_id, std::move(callback));
  } else if (*name == "browser_screenshot") {
    CallBrowserScreenshot(*args, request_id, std::move(callback));
  } else if (*name == "browser_execute_javascript") {
    CallBrowserExecuteJavascript(*args, request_id, std::move(callback));
  } else {
    SendJsonRpcError(request_id, kMethodNotFound,
                     "Unknown tool: " + *name, std::move(callback));
  }
}

void AbpMcpHandler::CallBrowserGetStatus(const base::Value::Dict& args,
                                         int request_id,
                                         McpResponseCallback callback) {
  controller_->GetBrowserStatus(
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserListTabs(const base::Value::Dict& args,
                                        int request_id,
                                        McpResponseCallback callback) {
  controller_->HandleRequest(
      "GET", "/api/v1/tabs", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserNewTab(const base::Value::Dict& args,
                                      int request_id,
                                      McpResponseCallback callback) {
  std::string body;
  base::JSONWriter::Write(base::Value(args.Clone()), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserCloseTab(const base::Value::Dict& args,
                                        int request_id,
                                        McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "DELETE", "/api/v1/tabs/" + *tab_id, "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGetTabInfo(const base::Value::Dict& args,
                                          int request_id,
                                          McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "GET", "/api/v1/tabs/" + *tab_id, "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserNavigate(const base::Value::Dict& args,
                                        int request_id,
                                        McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  base::Value::Dict body_dict;
  if (const std::string* url = args.FindString("url")) {
    body_dict.Set("url", *url);
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/navigate", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGoBack(const base::Value::Dict& args,
                                      int request_id,
                                      McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/back", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGoForward(const base::Value::Dict& args,
                                         int request_id,
                                         McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/forward", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserReload(const base::Value::Dict& args,
                                      int request_id,
                                      McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/reload", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserClick(const base::Value::Dict& args,
                                     int request_id,
                                     McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  base::Value::Dict body_dict;
  if (auto x = args.FindDouble("x")) {
    body_dict.Set("x", *x);
  }
  if (auto y = args.FindDouble("y")) {
    body_dict.Set("y", *y);
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/click", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserType(const base::Value::Dict& args,
                                    int request_id,
                                    McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  base::Value::Dict body_dict;
  if (const std::string* text = args.FindString("text")) {
    body_dict.Set("text", *text);
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/type", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserScreenshot(const base::Value::Dict& args,
                                          int request_id,
                                          McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  base::Value::Dict body_dict;
  base::Value::Dict screenshot_opts;
  if (const std::string* markup = args.FindString("markup")) {
    screenshot_opts.Set("markup", *markup);
  }
  if (const std::string* format = args.FindString("format")) {
    screenshot_opts.Set("format", *format);
  }
  body_dict.Set("screenshot", std::move(screenshot_opts));

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/screenshot", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserExecuteJavascript(const base::Value::Dict& args,
                                                 int request_id,
                                                 McpResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  base::Value::Dict body_dict;
  if (const std::string* expression = args.FindString("expression")) {
    body_dict.Set("script", *expression);
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/execute", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::OnControllerResponse(int request_id,
                                         McpResponseCallback callback,
                                         int status,
                                         const std::string& content_type,
                                         std::string body) {
  // Convert REST API response to MCP tool result
  // The REST API returns: {"success": true, "data": {...}}
  // We need to convert to: {"content": [{"type": "text", "text": "..."}]}

  base::Value::Dict result;
  base::Value::List content;
  base::Value::Dict text_content;
  text_content.Set("type", "text");

  // Parse the REST response
  auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (parsed && parsed->is_dict()) {
    const base::Value::Dict& response = parsed->GetDict();

    // Check if it's a success response with data
    if (const base::Value* data = response.Find("data")) {
      std::string data_json;
      base::JSONWriter::WriteWithOptions(
          *data, base::JSONWriter::OPTIONS_PRETTY_PRINT, &data_json);
      text_content.Set("text", data_json);
    } else {
      // Just return the whole response
      text_content.Set("text", body);
    }
  } else {
    // Not JSON, return as-is
    text_content.Set("text", body);
  }

  content.Append(std::move(text_content));
  result.Set("content", std::move(content));

  // If the REST call failed, mark as error
  if (status >= 400) {
    result.Set("isError", true);
  }

  SendJsonRpcResult(request_id, base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::SendJsonRpcResult(int request_id,
                                      base::Value result,
                                      McpResponseCallback callback) {
  base::Value::Dict response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", request_id);
  response.Set("result", std::move(result));

  std::string response_json;
  base::JSONWriter::Write(base::Value(std::move(response)), &response_json);

  std::move(callback).Run(200, "application/json", response_json);
}

void AbpMcpHandler::SendJsonRpcError(int request_id,
                                     int error_code,
                                     const std::string& message,
                                     McpResponseCallback callback) {
  base::Value::Dict response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", request_id);

  base::Value::Dict error;
  error.Set("code", error_code);
  error.Set("message", message);
  response.Set("error", std::move(error));

  std::string response_json;
  base::JSONWriter::Write(base::Value(std::move(response)), &response_json);

  std::move(callback).Run(200, "application/json", response_json);
}

void AbpMcpHandler::SendAccepted(McpResponseCallback callback) {
  std::move(callback).Run(202, "application/json", "");
}

std::string AbpMcpHandler::CreateSession(const std::string& client_name,
                                         const std::string& client_version) {
  // Cleanup expired sessions first
  CleanupExpiredSessions();

  auto session = std::make_unique<McpSession>();
  session->id = GenerateSessionId();
  session->client_name = client_name;
  session->client_version = client_version;
  session->created_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  session->last_activity_ms = session->created_at_ms;
  session->initialized = true;

  std::string id = session->id;
  sessions_[id] = std::move(session);
  return id;
}

McpSession* AbpMcpHandler::GetSession(const std::string& session_id) {
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return nullptr;
  }

  // Update last activity
  it->second->last_activity_ms =
      base::Time::Now().InMillisecondsSinceUnixEpoch();
  return it->second.get();
}

void AbpMcpHandler::CleanupExpiredSessions() {
  int64_t now_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();

  std::vector<std::string> expired;
  for (const auto& pair : sessions_) {
    if (now_ms - pair.second->last_activity_ms > kSessionTimeoutMs) {
      expired.push_back(pair.first);
    }
  }

  for (const auto& id : expired) {
    LOG(INFO) << "ABP MCP: Cleaning up expired session " << id;
    sessions_.erase(id);
  }
}

}  // namespace abp
