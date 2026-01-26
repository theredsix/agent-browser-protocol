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
    base::Value::Dict active_prop;
    active_prop.Set("type", "boolean");
    active_prop.Set("description", "Whether to activate the new tab");
    props.Set("active", std::move(active_prop));
    base::Value::Dict index_prop;
    index_prop.Set("type", "number");
    index_prop.Set("description", "Position in tab strip");
    props.Set("index", std::move(index_prop));
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
    base::Value::Dict referrer_prop;
    referrer_prop.Set("type", "string");
    referrer_prop.Set("description", "Referrer URL");
    props.Set("referrer", std::move(referrer_prop));
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
    base::Value::Dict ignore_cache_prop;
    ignore_cache_prop.Set("type", "boolean");
    ignore_cache_prop.Set("description", "Force refresh ignoring cache");
    props.Set("ignore_cache", std::move(ignore_cache_prop));
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
    base::Value::Dict button_prop;
    button_prop.Set("type", "string");
    button_prop.Set("description", "Mouse button: left, right, middle");
    base::Value::List button_enum;
    button_enum.Append("left");
    button_enum.Append("right");
    button_enum.Append("middle");
    button_prop.Set("enum", std::move(button_enum));
    props.Set("button", std::move(button_prop));
    base::Value::Dict click_count_prop;
    click_count_prop.Set("type", "number");
    click_count_prop.Set("description", "1=single, 2=double, 3=triple click");
    props.Set("click_count", std::move(click_count_prop));
    base::Value::Dict modifiers_prop;
    modifiers_prop.Set("type", "array");
    base::Value::Dict modifier_items;
    modifier_items.Set("type", "string");
    base::Value::List modifier_enum;
    modifier_enum.Append("Shift");
    modifier_enum.Append("Control");
    modifier_enum.Append("Alt");
    modifier_enum.Append("Meta");
    modifier_items.Set("enum", std::move(modifier_enum));
    modifiers_prop.Set("items", std::move(modifier_items));
    modifiers_prop.Set("description", "Modifier keys to hold during click");
    props.Set("modifiers", std::move(modifiers_prop));
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
    base::Value::Dict delay_prop;
    delay_prop.Set("type", "number");
    delay_prop.Set("description", "Delay between keystrokes in milliseconds");
    props.Set("delay_ms", std::move(delay_prop));
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
    base::Value::Dict format_prop;
    format_prop.Set("type", "string");
    format_prop.Set("description", "Image format: png, webp, jpeg");
    props.Set("format", std::move(format_prop));
    base::Value::Dict area_prop;
    area_prop.Set("type", "string");
    area_prop.Set("description", "Capture area: none, viewport");
    props.Set("area", std::move(area_prop));
    base::Value::Dict cursor_prop;
    cursor_prop.Set("type", "boolean");
    cursor_prop.Set("description", "Include virtual cursor in screenshot");
    props.Set("cursor", std::move(cursor_prop));
    base::Value::Dict full_page_prop;
    full_page_prop.Set("type", "boolean");
    full_page_prop.Set("description", "Capture full scrollable page");
    props.Set("full_page", std::move(full_page_prop));
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
    base::Value::Dict await_prop;
    await_prop.Set("type", "boolean");
    await_prop.Set("description", "Wait for promise resolution");
    props.Set("await_promise", std::move(await_prop));
    base::Value::Dict timeout_prop;
    timeout_prop.Set("type", "number");
    timeout_prop.Set("description", "Timeout for promise resolution in ms");
    props.Set("timeout_ms", std::move(timeout_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    required.Append("expression");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_keyboard_press
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_keyboard_press");
    tool.Set("description",
             "Press a key or key combination (e.g., Enter, Escape, Ctrl+C)");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    base::Value::Dict key_prop;
    key_prop.Set("type", "string");
    key_prop.Set("description",
                 "Key to press (e.g., Enter, Escape, a, F1, Tab)");
    props.Set("key", std::move(key_prop));
    base::Value::Dict modifiers_prop;
    modifiers_prop.Set("type", "array");
    base::Value::Dict modifier_items;
    modifier_items.Set("type", "string");
    base::Value::List modifier_enum;
    modifier_enum.Append("Shift");
    modifier_enum.Append("Control");
    modifier_enum.Append("Alt");
    modifier_enum.Append("Meta");
    modifier_items.Set("enum", std::move(modifier_enum));
    modifiers_prop.Set("items", std::move(modifier_items));
    modifiers_prop.Set("description", "Modifier keys to hold during press");
    props.Set("modifiers", std::move(modifiers_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    required.Append("key");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_scroll
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_scroll");
    tool.Set("description", "Scroll the page using mouse wheel");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    base::Value::Dict x_prop;
    x_prop.Set("type", "number");
    x_prop.Set("description", "X coordinate for scroll position");
    props.Set("x", std::move(x_prop));
    base::Value::Dict y_prop;
    y_prop.Set("type", "number");
    y_prop.Set("description", "Y coordinate for scroll position");
    props.Set("y", std::move(y_prop));
    base::Value::Dict delta_x_prop;
    delta_x_prop.Set("type", "number");
    delta_x_prop.Set("description", "Horizontal scroll amount (negative = left)");
    props.Set("delta_x", std::move(delta_x_prop));
    base::Value::Dict delta_y_prop;
    delta_y_prop.Set("type", "number");
    delta_y_prop.Set("description", "Vertical scroll amount (negative = down)");
    props.Set("delta_y", std::move(delta_y_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    required.Append("delta_y");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_mouse_move
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_mouse_move");
    tool.Set("description", "Move mouse to coordinates (for hover effects)");
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
    base::Value::Dict steps_prop;
    steps_prop.Set("type", "number");
    steps_prop.Set("description", "Intermediate steps for smooth movement");
    props.Set("steps", std::move(steps_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    required.Append("x");
    required.Append("y");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_activate_tab
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_activate_tab");
    tool.Set("description", "Switch to a specific tab");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "ID of tab to activate");
    props.Set("tab_id", std::move(tab_id_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_stop_loading
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_stop_loading");
    tool.Set("description", "Stop page loading");
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

  // browser_get_dialog
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_get_dialog");
    tool.Set("description",
             "Check if a dialog (alert/confirm/prompt) is pending");
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

  // browser_accept_dialog
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_accept_dialog");
    tool.Set("description", "Accept (click OK on) a pending dialog");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    base::Value::Dict prompt_text_prop;
    prompt_text_prop.Set("type", "string");
    prompt_text_prop.Set("description", "Text to enter for prompt dialogs");
    props.Set("prompt_text", std::move(prompt_text_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_dismiss_dialog
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_dismiss_dialog");
    tool.Set("description", "Dismiss (click Cancel on) a pending dialog");
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

  // browser_list_downloads
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_list_downloads");
    tool.Set("description", "List all downloads");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict state_prop;
    state_prop.Set("type", "string");
    base::Value::List state_enum;
    state_enum.Append("in_progress");
    state_enum.Append("completed");
    state_enum.Append("cancelled");
    state_enum.Append("failed");
    state_prop.Set("enum", std::move(state_enum));
    state_prop.Set("description", "Filter by download state");
    props.Set("state", std::move(state_prop));
    base::Value::Dict limit_prop;
    limit_prop.Set("type", "number");
    limit_prop.Set("description", "Maximum number of downloads to return");
    props.Set("limit", std::move(limit_prop));
    input_schema.Set("properties", std::move(props));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_get_download
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_get_download");
    tool.Set("description", "Get download status");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict download_id_prop;
    download_id_prop.Set("type", "string");
    download_id_prop.Set("description", "Download ID");
    props.Set("download_id", std::move(download_id_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("download_id");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_cancel_download
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_cancel_download");
    tool.Set("description", "Cancel an in-progress download");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict download_id_prop;
    download_id_prop.Set("type", "string");
    download_id_prop.Set("description", "Download ID");
    props.Set("download_id", std::move(download_id_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("download_id");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_provide_files
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_provide_files");
    tool.Set("description", "Provide files to a pending file chooser dialog");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict chooser_id_prop;
    chooser_id_prop.Set("type", "string");
    chooser_id_prop.Set("description", "File chooser ID from event");
    props.Set("chooser_id", std::move(chooser_id_prop));
    base::Value::Dict files_prop;
    files_prop.Set("type", "array");
    base::Value::Dict file_items;
    file_items.Set("type", "string");
    files_prop.Set("items", std::move(file_items));
    files_prop.Set("description", "File paths to provide");
    props.Set("files", std::move(files_prop));
    base::Value::Dict path_prop;
    path_prop.Set("type", "string");
    path_prop.Set("description", "Save path for save dialogs");
    props.Set("path", std::move(path_prop));
    base::Value::Dict cancel_prop;
    cancel_prop.Set("type", "boolean");
    cancel_prop.Set("description", "Cancel the file chooser");
    props.Set("cancel", std::move(cancel_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("chooser_id");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_keyboard_down
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_keyboard_down");
    tool.Set("description", "Press and hold a key");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    base::Value::Dict key_prop;
    key_prop.Set("type", "string");
    key_prop.Set("description", "Key to press down");
    props.Set("key", std::move(key_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    required.Append("key");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_keyboard_up
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_keyboard_up");
    tool.Set("description", "Release a held key");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    base::Value::Dict key_prop;
    key_prop.Set("type", "string");
    key_prop.Set("description", "Key to release");
    props.Set("key", std::move(key_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    required.Append("key");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_get_execution_state
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_get_execution_state");
    tool.Set("description", "Get JavaScript execution state for a tab");
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

  // browser_set_execution_state
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_set_execution_state");
    tool.Set("description", "Pause or resume JavaScript execution");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict tab_id_prop;
    tab_id_prop.Set("type", "string");
    tab_id_prop.Set("description", "Target tab ID");
    props.Set("tab_id", std::move(tab_id_prop));
    base::Value::Dict paused_prop;
    paused_prop.Set("type", "boolean");
    paused_prop.Set("description", "True to pause, false to resume");
    props.Set("paused", std::move(paused_prop));
    input_schema.Set("properties", std::move(props));
    base::Value::List required;
    required.Append("tab_id");
    required.Append("paused");
    input_schema.Set("required", std::move(required));
    tool.Set("inputSchema", std::move(input_schema));
    tools.Append(std::move(tool));
  }

  // browser_shutdown
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_shutdown");
    tool.Set("description", "Gracefully shut down the browser");
    base::Value::Dict input_schema;
    input_schema.Set("type", "object");
    base::Value::Dict props;
    base::Value::Dict timeout_prop;
    timeout_prop.Set("type", "number");
    timeout_prop.Set("description", "Timeout before force quit in ms");
    props.Set("timeout_ms", std::move(timeout_prop));
    input_schema.Set("properties", std::move(props));
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
                                     McpResponseCallback callback) {
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
                                    McpResponseCallback callback) {
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
                                          McpResponseCallback callback) {
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
                                                 McpResponseCallback callback) {
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
                                             McpResponseCallback callback) {
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
                                      McpResponseCallback callback) {
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
                                         McpResponseCallback callback) {
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
                                           McpResponseCallback callback) {
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
                                           McpResponseCallback callback) {
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
                                         McpResponseCallback callback) {
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
                                            McpResponseCallback callback) {
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
                                             McpResponseCallback callback) {
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
                                             McpResponseCallback callback) {
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
                                           McpResponseCallback callback) {
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
                                              McpResponseCallback callback) {
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
                                            McpResponseCallback callback) {
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
                                            McpResponseCallback callback) {
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
                                          McpResponseCallback callback) {
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
                                                 McpResponseCallback callback) {
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
                                                 McpResponseCallback callback) {
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
                                        McpResponseCallback callback) {
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
                                         McpResponseCallback callback,
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
