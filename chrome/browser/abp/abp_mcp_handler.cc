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
#include "chrome/browser/abp/abp_tool_builder.h"

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

// Get tool definitions for tools/list using ToolBuilder
base::Value::List GetToolDefinitions() {
  base::Value::List tools;

  // Browser status and tab management
  tools.Append(ToolBuilder("browser_get_status")
                   .Description("Get browser status and readiness")
                   .Build());

  tools.Append(ToolBuilder("browser_list_tabs")
                   .Description("List all open browser tabs")
                   .Build());

  tools.Append(ToolBuilder("browser_new_tab")
                   .Description("Create a new browser tab")
                   .OptionalString("url", "URL to navigate to")
                   .OptionalBoolean("active", "Whether to activate the new tab")
                   .OptionalNumber("index", "Position in tab strip")
                   .Build());

  tools.Append(ToolBuilder("browser_close_tab")
                   .Description("Close a browser tab")
                   .RequiredString("tab_id", "ID of tab to close")
                   .Build());

  tools.Append(ToolBuilder("browser_get_tab_info")
                   .Description("Get detailed information about a tab")
                   .RequiredString("tab_id", "ID of tab")
                   .Build());

  // Navigation
  tools.Append(ToolBuilder("browser_navigate")
                   .Description("Navigate to a URL")
                   .RequiredString("tab_id", "Target tab ID")
                   .RequiredString("url", "URL to navigate to")
                   .OptionalString("referrer", "Referrer URL")
                   .Build());

  tools.Append(ToolBuilder("browser_go_back")
                   .Description("Navigate back in history")
                   .RequiredString("tab_id", "Target tab ID")
                   .Build());

  tools.Append(ToolBuilder("browser_go_forward")
                   .Description("Navigate forward in history")
                   .RequiredString("tab_id", "Target tab ID")
                   .Build());

  tools.Append(ToolBuilder("browser_reload")
                   .Description("Reload the current page")
                   .RequiredString("tab_id", "Target tab ID")
                   .OptionalBoolean("ignore_cache", "Force refresh ignoring cache")
                   .Build());

  // Input actions
  tools.Append(ToolBuilder("browser_click")
                   .Description("Click at coordinates on the page")
                   .RequiredString("tab_id", "Target tab ID")
                   .RequiredNumber("x", "X coordinate")
                   .RequiredNumber("y", "Y coordinate")
                   .OptionalStringEnum("button", "Mouse button",
                                       {"left", "right", "middle"})
                   .OptionalNumber("click_count", "1=single, 2=double, 3=triple click")
                   .OptionalStringArrayEnum("modifiers", "Modifier keys to hold",
                                            {"Shift", "Control", "Alt", "Meta"})
                   .Build());

  tools.Append(ToolBuilder("browser_type")
                   .Description("Type text at current focus position")
                   .RequiredString("tab_id", "Target tab ID")
                   .RequiredString("text", "Text to type")
                   .OptionalNumber("delay_ms", "Delay between keystrokes in ms")
                   .Build());

  tools.Append(
      ToolBuilder("browser_screenshot")
          .Description("Take a screenshot of the page")
          .RequiredString("tab_id", "Target tab ID")
          .OptionalString("markup",
                          "Element markup overlay: none, interactive, "
                          "clickable, typeable, inputs")
          .OptionalString("format", "Image format: png, webp, jpeg")
          .OptionalString("area", "Capture area: none, viewport")
          .OptionalBoolean("cursor", "Include virtual cursor in screenshot")
          .OptionalBoolean("full_page", "Capture full scrollable page")
          .Build());

  tools.Append(
      ToolBuilder("browser_execute_javascript")
          .Description("Execute JavaScript in the page context")
          .RequiredString("tab_id", "Target tab ID")
          .RequiredString("expression", "JavaScript expression to evaluate")
          .OptionalBoolean("await_promise", "Wait for promise resolution")
          .OptionalNumber("timeout_ms", "Timeout for promise resolution in ms")
          .Build());

  tools.Append(
      ToolBuilder("browser_keyboard_press")
          .Description(
              "Press a key or key combination (e.g., Enter, Escape, Ctrl+C)")
          .RequiredString("tab_id", "Target tab ID")
          .RequiredString("key", "Key to press (e.g., Enter, Escape, a, F1, Tab)")
          .OptionalStringArrayEnum("modifiers", "Modifier keys to hold",
                                   {"Shift", "Control", "Alt", "Meta"})
          .Build());

  tools.Append(ToolBuilder("browser_scroll")
                   .Description("Scroll the page using mouse wheel")
                   .RequiredString("tab_id", "Target tab ID")
                   .RequiredNumber("delta_y", "Vertical scroll amount")
                   .OptionalNumber("x", "X coordinate for scroll position")
                   .OptionalNumber("y", "Y coordinate for scroll position")
                   .OptionalNumber("delta_x", "Horizontal scroll amount")
                   .Build());

  tools.Append(ToolBuilder("browser_mouse_move")
                   .Description("Move mouse to coordinates (for hover effects)")
                   .RequiredString("tab_id", "Target tab ID")
                   .RequiredNumber("x", "X coordinate")
                   .RequiredNumber("y", "Y coordinate")
                   .OptionalNumber("steps", "Intermediate steps for smooth movement")
                   .Build());

  // Tab control
  tools.Append(ToolBuilder("browser_activate_tab")
                   .Description("Switch to a specific tab")
                   .RequiredString("tab_id", "ID of tab to activate")
                   .Build());

  tools.Append(ToolBuilder("browser_stop_loading")
                   .Description("Stop page loading")
                   .RequiredString("tab_id", "Target tab ID")
                   .Build());

  // Dialog handling
  tools.Append(
      ToolBuilder("browser_get_dialog")
          .Description("Check if a dialog (alert/confirm/prompt) is pending")
          .RequiredString("tab_id", "Target tab ID")
          .Build());

  tools.Append(ToolBuilder("browser_accept_dialog")
                   .Description("Accept (click OK on) a pending dialog")
                   .RequiredString("tab_id", "Target tab ID")
                   .OptionalString("prompt_text", "Text to enter for prompt dialogs")
                   .Build());

  tools.Append(ToolBuilder("browser_dismiss_dialog")
                   .Description("Dismiss (click Cancel on) a pending dialog")
                   .RequiredString("tab_id", "Target tab ID")
                   .Build());

  // Downloads
  tools.Append(
      ToolBuilder("browser_list_downloads")
          .Description("List all downloads")
          .OptionalStringEnum("state", "Filter by download state",
                              {"in_progress", "completed", "cancelled", "failed"})
          .OptionalNumber("limit", "Maximum number of downloads to return")
          .Build());

  tools.Append(ToolBuilder("browser_get_download")
                   .Description("Get download status")
                   .RequiredString("download_id", "Download ID")
                   .Build());

  tools.Append(ToolBuilder("browser_cancel_download")
                   .Description("Cancel an in-progress download")
                   .RequiredString("download_id", "Download ID")
                   .Build());

  // File chooser
  tools.Append(ToolBuilder("browser_provide_files")
                   .Description("Provide files to a pending file chooser dialog")
                   .RequiredString("chooser_id", "File chooser ID from event")
                   .OptionalStringArray("files", "File paths to provide")
                   .OptionalString("path", "Save path for save dialogs")
                   .OptionalBoolean("cancel", "Cancel the file chooser")
                   .Build());

  // Keyboard hold/release
  tools.Append(ToolBuilder("browser_keyboard_down")
                   .Description("Press and hold a key")
                   .RequiredString("tab_id", "Target tab ID")
                   .RequiredString("key", "Key to press down")
                   .Build());

  tools.Append(ToolBuilder("browser_keyboard_up")
                   .Description("Release a held key")
                   .RequiredString("tab_id", "Target tab ID")
                   .RequiredString("key", "Key to release")
                   .Build());

  // Execution control
  tools.Append(ToolBuilder("browser_get_execution_state")
                   .Description("Get JavaScript execution state for a tab")
                   .RequiredString("tab_id", "Target tab ID")
                   .Build());

  tools.Append(ToolBuilder("browser_set_execution_state")
                   .Description("Pause or resume JavaScript execution")
                   .RequiredString("tab_id", "Target tab ID")
                   .RequiredBoolean("paused", "True to pause, false to resume")
                   .Build());

  // Browser control
  tools.Append(ToolBuilder("browser_shutdown")
                   .Description("Gracefully shut down the browser")
                   .OptionalNumber("timeout_ms", "Timeout before force quit in ms")
                   .Build());

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
    ResponseCallback callback) {
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
                                     ResponseCallback callback) {
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
                                    ResponseCallback callback) {
  base::Value::Dict result;
  result.Set("tools", GetToolDefinitions());

  SendJsonRpcResult(request_id, base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::HandleToolsCall(const base::Value::Dict& params,
                                    int request_id,
                                    ResponseCallback callback) {
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
  } else if (*name == "browser_keyboard_press") {
    CallBrowserKeyboardPress(*args, request_id, std::move(callback));
  } else if (*name == "browser_scroll") {
    CallBrowserScroll(*args, request_id, std::move(callback));
  } else if (*name == "browser_mouse_move") {
    CallBrowserMouseMove(*args, request_id, std::move(callback));
  } else if (*name == "browser_activate_tab") {
    CallBrowserActivateTab(*args, request_id, std::move(callback));
  } else if (*name == "browser_stop_loading") {
    CallBrowserStopLoading(*args, request_id, std::move(callback));
  } else if (*name == "browser_get_dialog") {
    CallBrowserGetDialog(*args, request_id, std::move(callback));
  } else if (*name == "browser_accept_dialog") {
    CallBrowserAcceptDialog(*args, request_id, std::move(callback));
  } else if (*name == "browser_dismiss_dialog") {
    CallBrowserDismissDialog(*args, request_id, std::move(callback));
  } else if (*name == "browser_list_downloads") {
    CallBrowserListDownloads(*args, request_id, std::move(callback));
  } else if (*name == "browser_get_download") {
    CallBrowserGetDownload(*args, request_id, std::move(callback));
  } else if (*name == "browser_cancel_download") {
    CallBrowserCancelDownload(*args, request_id, std::move(callback));
  } else if (*name == "browser_provide_files") {
    CallBrowserProvideFiles(*args, request_id, std::move(callback));
  } else if (*name == "browser_keyboard_down") {
    CallBrowserKeyboardDown(*args, request_id, std::move(callback));
  } else if (*name == "browser_keyboard_up") {
    CallBrowserKeyboardUp(*args, request_id, std::move(callback));
  } else if (*name == "browser_get_execution_state") {
    CallBrowserGetExecutionState(*args, request_id, std::move(callback));
  } else if (*name == "browser_set_execution_state") {
    CallBrowserSetExecutionState(*args, request_id, std::move(callback));
  } else if (*name == "browser_shutdown") {
    CallBrowserShutdown(*args, request_id, std::move(callback));
  } else {
    SendJsonRpcError(request_id, kMethodNotFound,
                     "Unknown tool: " + *name, std::move(callback));
  }
}

void AbpMcpHandler::CallBrowserGetStatus(const base::Value::Dict& args,
                                         int request_id,
                                         ResponseCallback callback) {
  controller_->GetBrowserStatus(
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserListTabs(const base::Value::Dict& args,
                                        int request_id,
                                        ResponseCallback callback) {
  controller_->HandleRequest(
      "GET", "/api/v1/tabs", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserNewTab(const base::Value::Dict& args,
                                      int request_id,
                                      ResponseCallback callback) {
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
                                        ResponseCallback callback) {
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
                                          ResponseCallback callback) {
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
                                        ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

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
                                      ResponseCallback callback) {
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
                                         ResponseCallback callback) {
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
                                      ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/reload", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserClick(const base::Value::Dict& args,
                                     int request_id,
                                     ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

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
                                    ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

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
                                          ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Build screenshot options from flat args
  base::Value::Dict body_dict;
  base::Value::Dict screenshot_opts;
  if (const std::string* markup = args.FindString("markup")) {
    screenshot_opts.Set("markup", *markup);
  }
  if (const std::string* format = args.FindString("format")) {
    screenshot_opts.Set("format", *format);
  }
  if (const std::string* area = args.FindString("area")) {
    screenshot_opts.Set("area", *area);
  }
  if (auto cursor = args.FindBool("cursor")) {
    screenshot_opts.Set("cursor", *cursor);
  }
  if (auto full_page = args.FindBool("full_page")) {
    screenshot_opts.Set("full_page", *full_page);
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
                                                 ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Map MCP "expression" to REST "script", forward other params
  base::Value::Dict body_dict;
  if (const std::string* expression = args.FindString("expression")) {
    body_dict.Set("script", *expression);
  }
  if (auto await_promise = args.FindBool("await_promise")) {
    body_dict.Set("await_promise", *await_promise);
  }
  if (auto timeout_ms = args.FindDouble("timeout_ms")) {
    body_dict.Set("timeout_ms", static_cast<int>(*timeout_ms));
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/execute", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserKeyboardPress(const base::Value::Dict& args,
                                             int request_id,
                                             ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/keyboard/press", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserScroll(const base::Value::Dict& args,
                                      int request_id,
                                      ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/scroll", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserMouseMove(const base::Value::Dict& args,
                                         int request_id,
                                         ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/move", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserActivateTab(const base::Value::Dict& args,
                                           int request_id,
                                           ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/activate", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserStopLoading(const base::Value::Dict& args,
                                           int request_id,
                                           ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/stop", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGetDialog(const base::Value::Dict& args,
                                         int request_id,
                                         ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "GET", "/api/v1/tabs/" + *tab_id + "/dialog", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserAcceptDialog(const base::Value::Dict& args,
                                            int request_id,
                                            ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/dialog/accept", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserDismissDialog(const base::Value::Dict& args,
                                             int request_id,
                                             ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/dialog/dismiss", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserListDownloads(const base::Value::Dict& args,
                                             int request_id,
                                             ResponseCallback callback) {
  // Build query string from optional params
  std::string path = "/api/v1/downloads";
  std::vector<std::string> query_parts;

  if (const std::string* state = args.FindString("state")) {
    query_parts.push_back("state=" + *state);
  }
  if (auto limit = args.FindDouble("limit")) {
    query_parts.push_back("limit=" + base::NumberToString(static_cast<int>(*limit)));
  }

  if (!query_parts.empty()) {
    path += "?";
    for (size_t i = 0; i < query_parts.size(); ++i) {
      if (i > 0) path += "&";
      path += query_parts[i];
    }
  }

  controller_->HandleRequest(
      "GET", path, "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGetDownload(const base::Value::Dict& args,
                                           int request_id,
                                           ResponseCallback callback) {
  const std::string* download_id = args.FindString("download_id");
  if (!download_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing download_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "GET", "/api/v1/downloads/" + *download_id, "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserCancelDownload(const base::Value::Dict& args,
                                              int request_id,
                                              ResponseCallback callback) {
  const std::string* download_id = args.FindString("download_id");
  if (!download_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing download_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/downloads/" + *download_id + "/cancel", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserProvideFiles(const base::Value::Dict& args,
                                            int request_id,
                                            ResponseCallback callback) {
  const std::string* chooser_id = args.FindString("chooser_id");
  if (!chooser_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing chooser_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("chooser_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/file-chooser/" + *chooser_id, body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserKeyboardDown(const base::Value::Dict& args,
                                            int request_id,
                                            ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/keyboard/down", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserKeyboardUp(const base::Value::Dict& args,
                                          int request_id,
                                          ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/keyboard/up", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGetExecutionState(const base::Value::Dict& args,
                                                 int request_id,
                                                 ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "GET", "/api/v1/tabs/" + *tab_id + "/execution", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserSetExecutionState(const base::Value::Dict& args,
                                                 int request_id,
                                                 ResponseCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id) {
    SendJsonRpcError(request_id, kInvalidParams, "Missing tab_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/execution", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserShutdown(const base::Value::Dict& args,
                                        int request_id,
                                        ResponseCallback callback) {
  // Forward all args to REST API
  std::string body;
  base::JSONWriter::Write(base::Value(args.Clone()), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/browser/shutdown", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void AbpMcpHandler::OnControllerResponse(int request_id,
                                         ResponseCallback callback,
                                         int status,
                                         const std::string& content_type,
                                         std::string body) {
  // Convert REST API response to MCP tool result
  // Return the COMPLETE REST response, including screenshot, scroll, events,
  // timing. MCP is a transport layer - don't strip data.

  base::Value::Dict result;
  base::Value::List content;
  base::Value::Dict text_content;
  text_content.Set("type", "text");

  // Parse and pretty-print the REST response
  auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (parsed && parsed->is_dict()) {
    // Return the complete REST response, not just data field
    std::string pretty_json;
    base::JSONWriter::WriteWithOptions(
        *parsed, base::JSONWriter::OPTIONS_PRETTY_PRINT, &pretty_json);
    text_content.Set("text", pretty_json);
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
                                      ResponseCallback callback) {
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
                                     ResponseCallback callback) {
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

void AbpMcpHandler::SendAccepted(ResponseCallback callback) {
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
