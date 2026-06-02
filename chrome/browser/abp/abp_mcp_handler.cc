// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_mcp_handler.h"

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/abp/abp_controller.h"
#include "chrome/browser/abp/abp_tool_builder.h"
#include "base/strings/escape.h"

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

// Get tool definitions for tools/list using ToolBuilder
base::Value::List GetToolDefinitions() {
  base::Value::List tools;

  // 1. browser_action — batched input actions (custom schema with oneOf
  //    discriminated variants per action type; ToolBuilder lacks array/oneOf)
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_action");
    tool.Set("description",
        "Execute 1-3 browser input actions in a single turn. You get one "
        "screenshot back after all actions complete. The page is paused "
        "between your tool calls — JS and animations only run during "
        "execution.\n\n"
        "Batch actions that form a single user intent:\n"
        "- click field + type text + press ENTER  (3 actions, 1 turn)\n"
        "- click field + type text                (2 actions, 1 turn)\n"
        "- standalone click or keypress           (1 action, 1 turn)\n\n"
        "Key names are ALL-CAPS: ENTER, TAB, ESCAPE, ARROWUP, etc.\n"
        "Abbreviations: CTRL, CMD, ESC, DEL, BS, CR, UP, DOWN, LEFT, RIGHT.\n"
        "Modifiers: SHIFT, CONTROL, ALT, META.\n\n"
        "Example — click a search box, type a query, press Enter:\n"
        "{\"actions\":[\n"
        "  {\"type\":\"mouse_click\",\"x\":350,\"y\":200},\n"
        "  {\"type\":\"keyboard_type\",\"text\":\"weather today\"},\n"
        "  {\"type\":\"keyboard_press\",\"key\":\"ENTER\"}\n"
        "]}");

    // -- Shared sub-schemas used across variants --
    base::Value::Dict num_prop;
    num_prop.Set("type", "number");

    base::Value::Dict str_prop;
    str_prop.Set("type", "string");

    // Modifiers array — reused by mouse_click and keyboard_press
    auto make_modifiers = []() {
      base::Value::Dict mods;
      mods.Set("type", "array");
      base::Value::Dict item;
      item.Set("type", "string");
      base::Value::List vals;
      vals.Append("SHIFT");
      vals.Append("CONTROL");
      vals.Append("ALT");
      vals.Append("META");
      item.Set("enum", std::move(vals));
      mods.Set("items", std::move(item));
      return mods;
    };

    // Helper: build a const-type property {"type":"string","const":"value"}
    auto make_const_type = [](const char* value) {
      base::Value::Dict d;
      d.Set("type", "string");
      d.Set("const", value);
      return d;
    };

    // -- oneOf variants --
    base::Value::List one_of;

    // mouse_click: x, y required; button?, click_count?, modifiers? optional
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("mouse_click"));
      props.Set("x", num_prop.Clone());
      props.Set("y", num_prop.Clone());

      base::Value::Dict button;
      button.Set("type", "string");
      base::Value::List btn_vals;
      btn_vals.Append("left");
      btn_vals.Append("right");
      btn_vals.Append("middle");
      button.Set("enum", std::move(btn_vals));
      props.Set("button", std::move(button));

      props.Set("click_count", num_prop.Clone());
      props.Set("modifiers", make_modifiers());

      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("x");
      req.Append("y");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }

    // keyboard_type: text required
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("keyboard_type"));
      props.Set("text", str_prop.Clone());
      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("text");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }

    // keyboard_press: key required; modifiers?, action? optional
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("keyboard_press"));
      props.Set("key", str_prop.Clone());
      props.Set("modifiers", make_modifiers());

      base::Value::Dict action_prop;
      action_prop.Set("type", "string");
      base::Value::List action_vals;
      action_vals.Append("press");
      action_vals.Append("down");
      action_vals.Append("up");
      action_prop.Set("enum", std::move(action_vals));
      props.Set("action", std::move(action_prop));

      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("key");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }

    // mouse_hover: x, y required
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("mouse_hover"));
      props.Set("x", num_prop.Clone());
      props.Set("y", num_prop.Clone());
      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("x");
      req.Append("y");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }

    // mouse_drag: start_x, start_y, end_x, end_y required; steps? optional
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("mouse_drag"));
      props.Set("start_x", num_prop.Clone());
      props.Set("start_y", num_prop.Clone());
      props.Set("end_x", num_prop.Clone());
      props.Set("end_y", num_prop.Clone());
      props.Set("steps", num_prop.Clone());
      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("start_x");
      req.Append("start_y");
      req.Append("end_x");
      req.Append("end_y");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }

    // -- actions array with oneOf items --
    base::Value::Dict actions_prop;
    actions_prop.Set("type", "array");
    base::Value::Dict items_schema;
    items_schema.Set("oneOf", std::move(one_of));
    actions_prop.Set("items", std::move(items_schema));
    actions_prop.Set("minItems", 1);
    actions_prop.Set("maxItems", 3);

    // -- Top-level properties --
    base::Value::Dict properties;
    properties.Set("actions", std::move(actions_prop));
    properties.Set("tab_id", str_prop.Clone());
    {
      base::Value::Dict ntag;
      ntag.Set("type", "string");
      ntag.Set("description",
               "Tag name to auto-save this action's network requests. "
                "Saved requests persist across actions and can be queried "
                "later by tag.");
      properties.Set("network_tag", std::move(ntag));
    }

    // screenshot config
    base::Value::Dict ss_prop;
    ss_prop.Set("type", "object");
    base::Value::Dict ss_props;

    base::Value::Dict markup_prop;
    markup_prop.Set("type", "array");
    base::Value::Dict markup_item;
    markup_item.Set("type", "string");
    base::Value::List markup_enum;
    markup_enum.Append("clickable");
    markup_enum.Append("typeable");
    markup_enum.Append("scrollable");
    markup_enum.Append("grid");
    markup_enum.Append("selected");
    markup_item.Set("enum", std::move(markup_enum));
    markup_prop.Set("items", std::move(markup_item));
    ss_props.Set("markup", std::move(markup_prop));

    base::Value::Dict disable_prop;
    disable_prop.Set("type", "array");
    base::Value::Dict disable_item;
    disable_item.Set("type", "string");
    disable_prop.Set("items", std::move(disable_item));
    ss_props.Set("disable_markup", std::move(disable_prop));

    base::Value::Dict format_prop;
    format_prop.Set("type", "string");
    base::Value::List format_enum;
    format_enum.Append("png");
    format_enum.Append("webp");
    format_enum.Append("jpeg");
    format_prop.Set("enum", std::move(format_enum));
    ss_props.Set("format", std::move(format_prop));

    ss_prop.Set("properties", std::move(ss_props));
    properties.Set("screenshot", std::move(ss_prop));

    base::Value::Dict schema;
    schema.Set("type", "object");
    schema.Set("properties", std::move(properties));
    base::Value::List required;
    required.Append("actions");
    schema.Set("required", std::move(required));

    tool.Set("inputSchema", std::move(schema));
    tools.Append(std::move(tool));
  }

  // 2. browser_scroll — scroll 1-3 viewports with a screenshot after each
  {
    base::Value::Dict scroll_item_props;
    {
      base::Value::Dict dp;
      dp.Set("type", "number");
      dp.Set("description",
             "Scroll amount in pixels (positive=down/right, negative=up/left)");
      scroll_item_props.Set("delta_px", std::move(dp));
    }
    {
      base::Value::Dict dir;
      dir.Set("type", "string");
      base::Value::List dir_enum;
      dir_enum.Append("x");
      dir_enum.Append("y");
      dir.Set("enum", std::move(dir_enum));
      dir.Set("description", "Scroll axis: 'x' for horizontal, 'y' for vertical");
      scroll_item_props.Set("direction", std::move(dir));
    }
    base::Value::List scroll_item_required;
    scroll_item_required.Append("delta_px");
    scroll_item_required.Append("direction");
    tools.Append(
        ToolBuilder("browser_scroll")
            .Description(
                "Scroll using mouse wheel at element coordinates. "
                "Simulates moving mouse over element and scrolling. "
                "Accepts a scrolls array of 1-3 {delta_px, direction} "
                "objects — a screenshot is captured after each scroll "
                "and returned as sequential image blocks. "
                "Use multiple scrolls (up to 3) when reading long pages "
                "or exploring vertically oriented filter panels, so you "
                "can gather several viewports of content in one action.")
            .OptionalString("tab_id", "Target tab ID")
            .RequiredNumber(
                "x",
                "X pixel coordinate of element center where mouse wheel "
                "fires. Read from the red grid on your screenshot. Must "
                "be within viewport bounds.")
            .RequiredNumber(
                "y",
                "Y pixel coordinate of element center where mouse wheel "
                "fires. Read from the red grid on your screenshot. Must "
                "be within viewport bounds.")
            .RequiredObjectArray(
                "scrolls",
                "Array of 1-3 scroll events. Each item scrolls from the "
                "same x,y coordinates. Pass multiple items to scroll "
                "multiple viewport heights in one action.",
                std::move(scroll_item_props),
                std::move(scroll_item_required))
            .OptionalString("network_tag",
                "Tag name to auto-save this action's network requests. "
                "Saved requests persist across actions and can be queried "
                "later by tag.")
            .Build());
  }

  // 3. browser_navigate — url or back/forward/reload
  tools.Append(ToolBuilder("browser_navigate")
                   .Description(
                       "Navigate to a URL, or go back/forward/reload. "
                       "Provide 'url' to navigate, or 'action' for "
                       "back/forward/reload.")
                   .OptionalString("tab_id", "Target tab ID")
                   .OptionalString("url", "URL to navigate to")
                   .OptionalStringEnum("action", "Navigation action",
                                       {"back", "forward", "reload"})
                   .OptionalString("network_tag",
                       "Tag name to auto-save this action's network requests. "
                "Saved requests persist across actions and can be queried "
                "later by tag.")
                   .Build());

  // 4. browser_screenshot
  tools.Append(
      ToolBuilder("browser_screenshot")
          .Description(
              "Take a screenshot. Also acts as a short wait: resumes page "
              "execution, waits up to 1 second for any action-triggered "
              "network requests to settle, captures the viewport, then "
              "re-pauses execution. Use this when you want to observe the "
              "current page state or give the page a brief moment to settle "
              "after an action. Use browser_wait instead when the page is "
              "actively loading (e.g. after navigation or form submission) "
              "and you need to wait up to 5 seconds for all in-flight "
              "network requests to complete.")
          .OptionalString("tab_id", "Target tab ID")
          .OptionalStringArrayEnum("disable_markup",
              "Markup overlays to disable. All overlays are enabled by "
              "default: clickable (green), typeable (orange), scrollable "
              "(purple dashed), grid (red coordinate grid), selected "
              "(blue, focused element)",
              {"clickable", "typeable", "scrollable", "grid", "selected"})
          .OptionalStringArrayEnum("markup",
              "Markup overlays to enable (none by default). clickable "
              "(green), typeable (orange), scrollable (purple dashed), "
              "grid (red coordinate grid), selected (blue, focused element)",
              {"clickable", "typeable", "scrollable", "grid", "selected"})
          .OptionalString("format", "Image format: png, webp, jpeg")
          .OptionalString("network_tag",
              "Tag name to auto-save this action's network requests. "
                "Saved requests persist across actions and can be queried "
                "later by tag.")
          .Build());

  // 4b. browser_wait — wait for network to settle
  tools.Append(
      ToolBuilder("browser_wait")
          .Description(
              "Wait for all in-flight same-site network requests to settle "
              "(up to 5 seconds), then capture a screenshot. Resumes page "
              "execution, tracks ALL currently in-flight requests (including "
              "those that started before this call), waits for them to "
              "complete, then re-pauses execution. Use this after navigation "
              "or form submission when the page is actively loading and you "
              "need to wait for network activity to finish. Use "
              "browser_screenshot instead when the page is already settled "
              "and you just want to observe its current state. Set "
              "animation=true on first wait after navigation to guarantee "
              "5s of page execution for CSS/JS animations.")
          .OptionalString("tab_id", "Target tab ID")
          .OptionalStringArrayEnum("markup",
              "Markup overlays to enable (none by default). clickable "
              "(green), typeable (orange), scrollable (purple dashed), "
              "grid (red coordinate grid), selected (blue, focused element)",
              {"clickable", "typeable", "scrollable", "grid", "selected"})
          .OptionalString("format", "Image format: png, webp, jpeg")
          .OptionalString("network_tag",
              "Tag name to auto-save this action's network requests. "
                "Saved requests persist across actions and can be queried "
                "later by tag.")
          .OptionalBoolean("animation",
              "Guarantees 5s of page execution for animations to play "
              "forward. Set true on first wait after navigation.")
          .Build());

  // 5. browser_tabs — list/new/close/info/activate/stop
  tools.Append(ToolBuilder("browser_tabs")
                   .Description(
                       "Manage browser tabs. Default: list all tabs. "
                       "Actions: list, new (create tab), close, info "
                       "(tab details), activate (switch to tab), stop "
                       "(stop loading).")
                   .OptionalStringEnum("action", "Tab action",
                       {"list", "new", "close", "info", "activate", "stop"})
                   .OptionalString("tab_id",
                       "Target tab ID (for close/info/activate/stop)")
                   .OptionalString("url", "URL for new tab")
                   .Build());

  // 6. browser_javascript
  tools.Append(
      ToolBuilder("browser_javascript")
          .Description(
              "Execute JavaScript in the page context. "
              "WARNING: Do NOT use this as a primary interaction method. "
              "Prefer browser_action (click, type, scroll) for all user "
              "interactions. Only use this tool for: (1) extracting data "
              "from the page (e.g. reading attributes, counting elements), "
              "or (2) locating elements when a mouse/keyboard action failed "
              "to produce the desired result and you need to inspect the DOM "
              "to understand why.")
          .OptionalString("tab_id", "Target tab ID")
          .RequiredString("expression", "JavaScript expression to evaluate")
          .OptionalString("network_tag",
              "Tag name to auto-save this action's network requests. "
                "Saved requests persist across actions and can be queried "
                "later by tag.")
          .Build());

  // 7. browser_text
  tools.Append(ToolBuilder("browser_text")
                   .Description("Get the visible text content of the page")
                   .OptionalString("tab_id", "Target tab ID")
                   .OptionalString("selector",
                       "CSS selector to scope text extraction")
                   .OptionalString("network_tag",
                       "Tag name to auto-save this action's network requests. "
                "Saved requests persist across actions and can be queried "
                "later by tag.")
                   .Build());

  // 8. browser_dialog — check/accept/dismiss
  tools.Append(
      ToolBuilder("browser_dialog")
          .Description(
              "Handle browser dialogs (alert/confirm/prompt). Default: "
              "check if a dialog is pending.")
          .OptionalString("tab_id", "Target tab ID")
          .OptionalStringEnum("action", "Dialog action",
                              {"check", "accept", "dismiss"})
          .OptionalString("prompt_text",
              "Text to enter for prompt dialogs (used with accept)")
          .Build());

  // 9. browser_downloads — list/status/cancel/content
  tools.Append(
      ToolBuilder("browser_downloads")
          .Description(
              "Manage downloads. Default: list all downloads. "
              "Use action 'content' to retrieve a downloaded file's "
              "binary content as a base64 blob.")
          .OptionalStringEnum("action", "Download action",
                              {"list", "status", "cancel", "content"})
          .OptionalString("download_id",
              "Download ID (required for status/cancel/content)")
          .OptionalStringEnum("state", "Filter by download state (for list)",
              {"in_progress", "completed", "cancelled", "failed"})
          .OptionalNumber("limit",
              "Maximum number of downloads to return (for list)")
          .OptionalNumber("max_size",
              "Maximum file size in bytes to return (for content, "
              "default 10MB)")
          .Build());

  // 10. browser_files
  tools.Append(ToolBuilder("browser_files")
                   .Description(
                       "Provide files to a pending file chooser dialog. "
                       "Use 'files' for local paths or 'content_files' for "
                       "base64-encoded file data (useful for remote clients). "
                       "content_files format: [{\"filename\": \"name.pdf\", "
                       "\"data\": \"<base64>\", \"mime_type\": \"...\"}]. "
                       "Both can be used together.")
                   .RequiredString("chooser_id", "File chooser ID from event")
                   .OptionalStringArray("files", "Local file paths to provide")
                   .OptionalString("path", "Save path for save dialogs")
                   .OptionalBoolean("cancel", "Cancel the file chooser")
                   .OptionalObjectArray("content_files",
                       "Base64-encoded files to upload (for remote clients)",
                       []{
                         base::Value::Dict props;
                         base::Value::Dict fn;
                         fn.Set("type", "string");
                         fn.Set("description", "Filename with extension");
                         props.Set("filename", std::move(fn));
                         base::Value::Dict dt;
                         dt.Set("type", "string");
                         dt.Set("description", "Base64-encoded file content");
                         props.Set("data", std::move(dt));
                         base::Value::Dict mt;
                         mt.Set("type", "string");
                         mt.Set("description", "MIME type (optional)");
                         props.Set("mime_type", std::move(mt));
                         return props;
                       }(),
                       []{
                         base::Value::List req;
                         req.Append("filename");
                         req.Append("data");
                         return req;
                       }())
                   .OptionalNumber("max_size",
                       "Maximum size per content_file in bytes (default 10MB)")
                   .Build());

  // 11. browser_select_picker
  tools.Append(
      ToolBuilder("browser_select_picker")
          .Description(
              "Respond to a pending select popup by choosing option(s). "
              "Use 'cancel' to dismiss without selecting.")
          .RequiredString("popup_id",
                          "The select popup ID from the select_open event")
          .OptionalIntegerArray("indices",
                                "Index(es) of option(s) to select")
          .OptionalBoolean("cancel",
                           "Dismiss the popup without selecting")
          .Build());

  // 11b. browser_datetime_picker
  tools.Append(
      ToolBuilder("browser_datetime_picker")
          .Description(
              "Respond to a date/time picker (from a datetime_picker_open "
              "event): choose an ISO value, or cancel to dismiss.")
          .RequiredString("popup_id",
                          "The 'id' from the datetime_picker_open event")
          .RequiredStringEnum(
              "action",
              "\"choose\" to apply a value, or \"cancel\" to dismiss the picker "
              "without choosing",
              {"choose", "cancel"})
          .OptionalString(
              "value",
              "ISO value to apply when action is \"choose\". The format must "
              "match the event's input_type: "
              "date -> \"2026-06-15\"; "
              "time -> \"14:45\" (24h HH:MM); "
              "datetime-local -> \"2026-06-15T14:45\"; "
              "month -> \"2026-06\"; "
              "week -> \"2026-W25\" (ISO week).")
          .Build());

  // 12. browser_get_status
  tools.Append(ToolBuilder("browser_get_status")
                   .Description("Get browser status and readiness")
                   .Build());

  // cdp_mode
  tools.Append(
      ToolBuilder("cdp_mode")
          .Description(
              "Enter or exit CDP mode for external browser control via Chrome "
              "DevTools Protocol. Enter suspends ABP and starts a CDP WebSocket "
              "server. Exit stops the server and returns control to ABP.")
          .RequiredStringEnum("action",
                              "Enter or exit CDP mode",
                              {"enter", "exit"})
          .OptionalNumber("port",
                          "CDP server port (default: auto-select starting at 24578)")
          .OptionalNumber("timeout_ms",
                          "Auto-exit timeout in milliseconds (no timeout if omitted)")
          .Build());

  // 14. browser_shutdown
  tools.Append(ToolBuilder("browser_shutdown")
                   .Description("Gracefully shut down the browser")
                   .OptionalNumber("timeout_ms",
                       "Timeout before force quit in ms")
                   .Build());

  // 15. browser_slider — standalone slider macro
  tools.Append(
      ToolBuilder("browser_slider")
          .Description(
              "Move a slider to a target value using linear "
              "interpolation over track geometry and value range. "
              "Provide orientation ('horizontal' or 'vertical'), "
              "track bounds, current thumb position, min/max values, "
              "and target_value. If the slider has a non-linear scale, "
              "custom styling, or this tool does not produce the "
              "desired result, fall back to browser_action with "
              "mouse_drag — click the thumb position and drag by the "
              "desired pixel offset.")
          .OptionalString("tab_id", "Target tab ID")
          .RequiredString("orientation",
              "Slider orientation: 'horizontal' (use y, x_start, x_end, "
              "current_x) or 'vertical' (use x, y_start, y_end, current_y)")
          .OptionalNumber("y",
              "Y coordinate of horizontal slider track (required for "
              "horizontal)")
          .OptionalNumber("x_start",
              "Left edge of horizontal track in pixels (required for "
              "horizontal)")
          .OptionalNumber("x_end",
              "Right edge of horizontal track in pixels (required for "
              "horizontal)")
          .OptionalNumber("current_x",
              "Current thumb X position in pixels (required for horizontal)")
          .OptionalNumber("x",
              "X coordinate of vertical slider track (required for vertical)")
          .OptionalNumber("y_start",
              "Top edge of vertical track in pixels (required for vertical)")
          .OptionalNumber("y_end",
              "Bottom edge of vertical track in pixels (required for "
              "vertical)")
          .OptionalNumber("current_y",
              "Current thumb Y position in pixels (required for vertical)")
          .RequiredNumber("min", "Minimum logical value of the slider")
          .RequiredNumber("max", "Maximum logical value of the slider")
          .RequiredNumber("target_value",
              "Desired logical value to set the slider to")
          .OptionalString("network_tag",
              "Tag name to auto-save this action's network requests. "
                "Saved requests persist across actions and can be queried "
                "later by tag.")
          .Build());

  // 16. browser_clear_text — clear focused input via backspace
  tools.Append(
      ToolBuilder("browser_clear_text")
          .Description(
              "Clear the text content of an input element by clicking to "
              "focus it, selecting all text, then pressing Backspace to "
              "delete the selection. Use this when you need to clear an "
              "input field before typing new text.")
          .OptionalString("tab_id", "Target tab ID")
          .RequiredNumber("x",
              "X coordinate of the center of the input field")
          .RequiredNumber("y",
              "Y coordinate of the center of the input field")
          .OptionalString("network_tag",
              "Tag name to auto-save this action's network requests. "
                "Saved requests persist across actions and can be queried "
                "later by tag.")
          .Build());

  // 17. respond_to_permission — handle permission prompts
  tools.Append(
      ToolBuilder("respond_to_permission")
          .Description(
              "Respond to a pending browser permission prompt. When a page "
              "requests a permission (e.g. geolocation), a "
              "'permission_requested' event is emitted with an ID. Use this "
              "tool to grant or deny that permission. When granting "
              "geolocation, provide latitude and longitude to set mock "
              "coordinates.")
          .RequiredString("permission_id",
                          "The permission request ID from the "
                          "permission_requested event")
          .RequiredString("permission_type",
                          "The permission type (e.g. 'geolocation'). Must "
                          "match the type in the permission_requested event.")
          .RequiredBoolean("allow", "True to grant, false to deny")
          .OptionalNumber("latitude",
                          "Latitude in degrees (-90 to 90). Required when "
                          "granting geolocation.")
          .OptionalNumber("longitude",
                          "Longitude in degrees (-180 to 180). Required when "
                          "granting geolocation.")
          .OptionalNumber("accuracy",
                          "Accuracy radius in meters (default: 100). Only "
                          "used when granting geolocation.")
          .Build());

  // 18. browser_network — query/save/clear captured network requests
  tools.Append(
      ToolBuilder("browser_network")
          .Description(
              "Query, save, or clear captured network requests. Every browser "
              "action automatically captures network requests (XHR, Fetch, and "
              "Document types by default).\n\n"
              "Captured requests from the most recent action are always "
              "available to query. Wait actions accumulate requests from prior "
              "actions; all other actions start with a fresh capture. To retain "
              "requests across actions, use save or network_tag on the "
              "action.\n\n"
              "action=\"query\": Search captured network requests. Supports "
              "filters on url, hostname, path, method, status, type, "
              "action_id, and tag. Use include_body=true to include request/"
              "response bodies.\n\n"
              "action=\"save\": Persist captured requests with a tag name so "
              "they remain available across future actions.\n\n"
              "action=\"clear\": Remove previously saved requests, optionally "
              "filtered by tag.")
          .RequiredStringEnum("action", "Operation: query, save, or clear",
                              {"query", "save", "clear"})
          .OptionalString("tag",
              "Tag name. For save: label this snapshot. For query/clear: "
              "filter by tag.")
          .OptionalString("tab_id",
              "Scope to a specific tab (query and save)")
          .OptionalString("url", "Regex filter on full URL (query only)")
          .OptionalString("hostname",
              "Regex filter on hostname (query only)")
          .OptionalString("path", "Regex filter on URL path (query only)")
          .OptionalString("query",
              "Regex filter on query string (query only)")
          .OptionalString("method",
              "Regex filter on HTTP method (query only)")
          .OptionalString("status",
              "Regex filter on HTTP status code (query only)")
          .OptionalString("type",
              "Resource type filter: document, stylesheet, script, image, "
              "font, xhr, fetch, websocket, other (query only)")
          .OptionalString("action_id",
              "Filter by action ID (query only)")
          .OptionalBoolean("include_body",
              "Include request/response bodies in results (default: false, "
              "query only)")
          .OptionalNumber("max_body_size",
              "Truncate request/response bodies to this many characters "
              "(0 = no limit, query only)")
          .Build());

  // 19. browser_curl — session-aware HTTP client using tab cookies
  tools.Append(
      ToolBuilder("browser_curl")
          .Description(
              "Execute an HTTP request using the tab's current session "
              "(cookies, auth tokens). Useful for calling APIs on the same "
              "site the tab is authenticated to.\n\n"
              "Returns text responses as text content, and image responses "
              "(image/* content types) as native MCP image content blocks "
              "so the model can view them directly.\n\n"
              "Use tag to persist the captured request/response to the "
              "network database.")
          .RequiredString("tab_id",
              "Tab whose cookies and session to use for the request")
          .RequiredString("url", "Request URL")
          .OptionalString("method",
              "HTTP method (default: GET)")
          .OptionalString("body", "Request body")
          .OptionalObject("headers",
              "Additional HTTP request headers as key-value string pairs "
              "(e.g. {\"Content-Type\": \"application/json\"})")
          .OptionalString("tag",
              "Tag name to persist this request to the network database")
          .Build());

  // 20. browser_console — query JavaScript console messages
  tools.Append(
      ToolBuilder("browser_console")
          .Description(
              "Query JavaScript console messages including logs, errors, "
              "warnings, and browser messages (CORS, CSP). Use to debug "
              "page behavior without requiring an action cycle.\n\n"
              "Returns buffered console messages with optional filters. "
              "Use after_id to poll for new messages since last query.\n\n"
              "Set clear=true to clear the buffer instead of querying.")
          .OptionalStringEnum("level",
              "Minimum severity level",
              {"verbose", "info", "warning", "error"})
          .OptionalString("pattern",
              "RE2 regex filter on message text (case-insensitive)")
          .OptionalString("tab_id",
              "Filter to specific tab ID")
          .OptionalNumber("limit",
              "Maximum entries to return (default 100)")
          .OptionalNumber("after_id",
              "Return only entries with id greater than this value")
          .OptionalBoolean("clear",
              "Clear the buffer instead of querying (uses tab_id if set)")
          .Build());

  return tools;
}

// Guide content served via resources/read for abp://guide
constexpr char kGuideContent[] = R"md(# ABP Browser Control Guide

## How ABP Works

ABP **pauses JavaScript and virtual time** between your actions. The page is frozen until your next tool call.

When you call any action tool (browser_action, browser_scroll, browser_navigate, etc.):
1. ABP **resumes** JS execution
2. ABP dispatches your action(s)
3. ABP **waits for the page to settle** (three-phase: 150ms for JS to fire handlers → tracks triggered network requests until they complete or 1s timeout → 150ms DOM settle)
4. ABP captures a **screenshot** automatically
5. ABP **re-pauses** JS execution
6. You receive the response with the screenshot

**One tool call = one complete turn.** Screenshots are included automatically with every action response. There is no need to take a separate screenshot after performing an action.

## Batching Actions

`browser_action` accepts 1-3 actions per call. Batch common workflows to reduce round-trips:

- **Click, type, submit:** `[{mouse_click, x, y}, {keyboard_type, text}, {keyboard_press, key: ENTER}]`
- **Click and type:** `[{mouse_click, x, y}, {keyboard_type, text}]`
- **Single click:** `[{mouse_click, x, y}]`

Actions execute sequentially with a 20ms pause between each. One screenshot is taken after all actions complete.

**Do NOT batch scrolling** — use `browser_scroll` separately.

## Waiting for Slow Content

ABP automatically tracks network requests triggered by your action and waits for them to complete (up to 1s for clicks, 60s for file uploads). If requests don't finish in time, a `request_tracking_timeout` event appears in the response — the screenshot may not reflect the final page state.

When the screenshot shows incomplete content, **call `browser_screenshot`** to wait and observe. It runs the same resume-wait-capture-pause cycle without performing any action, giving the page another chance to settle. Repeat until the content appears.

## Markup Overlays

Pass `markup: ["clickable", "typeable", "grid"]` to `browser_screenshot` to see labeled overlays on interactive elements. Each label shows the element's coordinates for targeting clicks and typing.

## Tool Reference (18 tools)

All `tab_id` parameters are optional and default to the active tab.

**Input:**
- `browser_action` — 1-3 actions: mouse_click (x, y), keyboard_type (text), keyboard_press (key, modifiers?), mouse_hover (x, y), mouse_drag (start_x, start_y, end_x, end_y). Keys are ALL-CAPS (ENTER, TAB, ESCAPE, CONTROL, META, etc.). Abbreviations accepted: CTRL, CMD, ESC, DEL.
- `browser_scroll` — x, y (where wheel fires), scrolls: required array of 1-3 `{delta_px, direction}` objects (direction: "x" or "y", delta_px positive=down/right, negative=up/left). Returns a screenshot after each scroll as sequential image blocks.
- `browser_slider` — orientation (horizontal/vertical), track bounds, current position, min, max, target_value. Calculates and executes drag automatically. Fallback chain if result is wrong: (1) `browser_action` with `mouse_drag`, (2) click the slider then use ARROWRIGHT/ARROWLEFT (or ARROWUP/ARROWDOWN) to nudge incrementally.
- `browser_clear_text` — x, y (center of input). Clicks to focus, selects all text, then presses Backspace to delete.

**Navigation:**
- `browser_navigate` — url? OR action? (back, forward, reload)
- `browser_tabs` — action? (list, new, close, info, activate, stop; default: list), tab_id?, url?

**Observation:**
- `browser_screenshot` — markup?, disable_markup?, format?
- `browser_javascript` — expression (required). Data extraction and DOM inspection ONLY — do NOT use for interaction; prefer browser_action.
- `browser_text` — selector?

**Situational:**
- `browser_dialog` — action? (check, accept, dismiss; default: check), prompt_text?
- `browser_downloads` — action? (list, status, cancel, content; default: list), download_id?, state?, limit?, max_size?. Use action:"content" with download_id to retrieve file bytes as base64 BlobResourceContents.
- `browser_files` — chooser_id (required), files?, content_files?, path?, cancel?, max_size?. Use content_files for base64 uploads: [{filename, data, mime_type}].
- `browser_select_picker` — popup_id (required), indices? (array of ints), cancel?. Respond to a pending <select> popup.
- `browser_datetime_picker` — popup_id (required), action (required: `choose`|`cancel`), value (ISO when action=choose; format per input_type: date `2026-06-15`, time `14:45`, datetime-local `2026-06-15T14:45`, month `2026-06`, week `2026-W25`). Respond to a date/time picker (datetime_picker_open event).
- `respond_to_permission` — permission_id (required), permission_type (required), allow (required), latitude?, longitude?, accuracy?. Respond to a permission prompt. When granting geolocation, provide latitude and longitude for mock coordinates.

**Browser:**
- `browser_get_status` — no params
- `browser_shutdown` — timeout_ms?

## Debugging

Session data is stored in the session directory (set via `--abp-session-dir` or defaults to `/tmp/abp-<UUID>/`):

```
sessions/<timestamp>/
├── history.db           # SQLite database with sessions, actions, events
└── screenshots/         # Auto-saved before/after WebP screenshots per action
```

Query the database:
```sql
-- Recent actions
SELECT id, type, status, url, error FROM actions ORDER BY id DESC LIMIT 10;
-- Events for an action
SELECT * FROM events WHERE action_id = <id>;
-- Screenshot paths
SELECT screenshot_before_path, screenshot_after_path FROM actions WHERE id = <id>;
```

## Best Practices

- **Use filters and sorting aggressively.** When a page offers filters (price range, category, date, ratings, size, color, etc.), sorting options, or faceted search — always apply them to narrow results before scrolling through content. This reduces the number of pages you need to process and gets to relevant results faster.
- **Prefer search over browsing.** If you know what you're looking for, use the site's search bar rather than clicking through menus.

## Tips

- `browser_javascript` uses `expression` as its parameter name (not `script`)
- `browser_scroll` requires `x`, `y` coordinates where the mouse wheel fires — target the element center
- Scroll direction: `delta_px` positive = scroll down/right, negative = scroll up/left (direction: "y" for vertical, "x" for horizontal)
- JS is paused between actions — timers and animations don't advance until your next tool call
- Key names are ALL-CAPS: ENTER, TAB, ESCAPE, BACKSPACE, ARROWUP, ARROWDOWN, etc.
- Modifier keys for keyboard_press: SHIFT, CONTROL, ALT, META (or abbreviations CTRL, CMD, OPT)
)md";

}  // namespace

AbpMcpHandler::AbpMcpHandler(AbpController* controller)
    : controller_(controller) {}

AbpMcpHandler::~AbpMcpHandler() = default;

std::string AbpMcpHandler::ResolveTabId(const base::Value::Dict& args) {
  const std::string* tab_id = args.FindString("tab_id");
  if (tab_id && !tab_id->empty()) {
    return *tab_id;
  }
  return controller_->GetActiveTabId();
}

void AbpMcpHandler::HandleRequest(
    const std::string& method,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    ResponseWithHeadersCallback callback) {
  // Handle GET request (SSE stream) - not implemented yet
  if (method == "GET") {
    // For now, return method not allowed
    // TODO: Implement SSE streaming for server notifications
    std::move(callback).Run(405, "application/json", {},
                            R"({"error":"SSE streaming not implemented"})");
    return;
  }

  // Handle DELETE request - no-op since sessions are not supported
  if (method == "DELETE") {
    std::move(callback).Run(204, "application/json", {}, "");
    return;
  }

  // Handle POST request (JSON-RPC)
  if (method != "POST") {
    std::move(callback).Run(405, "application/json", {},
                            R"({"error":"Method not allowed"})");
    return;
  }

  // Parse JSON-RPC request
  auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    SendJsonRpcError(base::Value(), kParseError, "Parse error",
                     std::move(callback));
    return;
  }

  const base::Value::Dict& request = parsed->GetDict();

  // Validate JSON-RPC version
  const std::string* jsonrpc = request.FindString("jsonrpc");
  if (!jsonrpc || *jsonrpc != "2.0") {
    SendJsonRpcError(base::Value(), kInvalidRequest,
                     "Invalid JSON-RPC version", std::move(callback));
    return;
  }

  // Get request ID - preserve original type (int, string, or null per JSON-RPC 2.0)
  base::Value request_id;
  if (const auto* id_value = request.Find("id")) {
    request_id = id_value->Clone();
  }

  // Get method
  const std::string* rpc_method = request.FindString("method");
  if (!rpc_method) {
    SendJsonRpcError(std::move(request_id), kInvalidRequest, "Missing method",
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
    HandleInitialize(*params, std::move(request_id), std::move(callback));
  } else if (*rpc_method == "notifications/initialized") {
    // Client notification that initialization is complete
    SendAccepted(std::move(callback));
  } else if (*rpc_method == "tools/list") {
    HandleToolsList(std::move(request_id), std::move(callback));
  } else if (*rpc_method == "tools/call") {
    HandleToolsCall(*params, std::move(request_id), std::move(callback));
  } else if (*rpc_method == "resources/list") {
    HandleResourcesList(std::move(request_id), std::move(callback));
  } else if (*rpc_method == "resources/read") {
    HandleResourcesRead(*params, std::move(request_id), std::move(callback));
  } else if (*rpc_method == "ping") {
    // Simple ping/pong
    base::Value::Dict result;
    SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                      std::move(callback));
  } else {
    SendJsonRpcError(std::move(request_id), kMethodNotFound, "Method not found",
                     std::move(callback));
  }
}

void AbpMcpHandler::HandleInitialize(const base::Value::Dict& params,
                                     base::Value request_id,
                                     ResponseWithHeadersCallback callback) {
  // Extract client info for logging
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

  LOG(INFO) << "ABP MCP: Initialize from client " << client_name << " v"
            << client_version;

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
  base::Value::Dict resources_cap;
  capabilities.Set("resources", std::move(resources_cap));
  result.Set("capabilities", std::move(capabilities));

  // Build JSON-RPC response
  base::Value::Dict response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", std::move(request_id));
  response.Set("result", std::move(result));

  std::string response_json;
  base::JSONWriter::Write(base::Value(std::move(response)), &response_json);

  std::move(callback).Run(200, "application/json", {}, response_json);
}

void AbpMcpHandler::HandleToolsList(base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  base::Value::Dict result;
  result.Set("tools", GetToolDefinitions());

  SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::HandleToolsCall(const base::Value::Dict& params,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  const std::string* name = params.FindString("name");
  if (!name) {
    SendJsonRpcError(std::move(request_id), kInvalidParams, "Missing tool name",
                     std::move(callback));
    return;
  }

  const base::Value::Dict* args = params.FindDict("arguments");
  base::Value::Dict empty_args;
  if (!args) {
    args = &empty_args;
  }

  // Block most tools in human or CDP mode — only allow read-only observation
  bool is_suspended_mode =
      controller_->GetInputMode() == AbpController::InputMode::kHuman ||
      controller_->GetInputMode() == AbpController::InputMode::kCdp;
  bool is_cdp_mode =
      controller_->GetInputMode() == AbpController::InputMode::kCdp;

  // Always allowed: status, screenshot, tabs, console, cdp_mode
  bool is_always_allowed =
      *name == "browser_get_status" || *name == "browser_screenshot" ||
      *name == "browser_tabs" || *name == "browser_console" ||
      *name == "cdp_mode";
  // Allowed in human mode only (not CDP mode): browser_text
  bool is_human_only_allowed =
      !is_cdp_mode && *name == "browser_text";

  if (is_suspended_mode && !is_always_allowed && !is_human_only_allowed) {
    std::string error_msg = is_cdp_mode
        ? "Operation blocked: browser is in cdp mode. "
          "Use browser_get_status to check current input_mode, or use "
          "cdp_mode with action 'exit' to return control to ABP."
        : "Operation blocked: browser is in human input mode. "
          "Use browser_get_status to check current input_mode, or set "
          "input_mode to 'agent' to switch back via "
          "POST /api/v1/browser/input-mode";
    base::Value::Dict result;
    base::Value::List content;
    base::Value::Dict text_block;
    text_block.Set("type", "text");
    text_block.Set("text", error_msg);
    content.Append(std::move(text_block));
    result.Set("content", std::move(content));
    result.Set("isError", true);
    SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                      std::move(callback));
    return;
  }

  // Route to tool implementation
  if (*name == "browser_action") {
    CallBrowserAction(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_scroll") {
    CallBrowserScroll(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_navigate") {
    CallBrowserNavigate(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_screenshot") {
    CallBrowserScreenshot(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_tabs") {
    CallBrowserTabs(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_javascript") {
    CallBrowserJavascript(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_text") {
    CallBrowserText(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_dialog") {
    CallBrowserDialog(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_downloads") {
    CallBrowserDownloads(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_files") {
    CallBrowserFiles(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_select_picker") {
    CallBrowserSelectPicker(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_datetime_picker") {
    CallBrowserDateTimePicker(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_get_status") {
    CallBrowserGetStatus(*args, std::move(request_id), std::move(callback));
  } else if (*name == "cdp_mode") {
    CallCdpMode(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_shutdown") {
    CallBrowserShutdown(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_slider") {
    CallBrowserSlider(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_clear_text") {
    CallBrowserClearText(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_wait") {
    CallBrowserWait(*args, std::move(request_id), std::move(callback));
  } else if (*name == "respond_to_permission") {
    CallRespondToPermission(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_network") {
    CallBrowserNetwork(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_curl") {
    CallBrowserCurl(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_console") {
    CallBrowserConsole(*args, std::move(request_id), std::move(callback));
  } else {
    SendJsonRpcError(std::move(request_id), kMethodNotFound,
                     "Unknown tool: " + *name, std::move(callback));
  }
}

void AbpMcpHandler::HandleResourcesList(base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  base::Value::Dict result;
  base::Value::List resources;

  base::Value::Dict guide;
  guide.Set("uri", "abp://guide");
  guide.Set("name", "ABP Usage Guide");
  guide.Set("description",
            "How to use ABP browser tools effectively - covers the "
            "pause/resume execution model, screenshot behavior, tool "
            "reference, and debugging");
  guide.Set("mimeType", "text/markdown");
  resources.Append(std::move(guide));

  result.Set("resources", std::move(resources));
  SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::HandleResourcesRead(const base::Value::Dict& params,
                                        base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  const std::string* uri = params.FindString("uri");
  if (!uri) {
    SendJsonRpcError(std::move(request_id), kInvalidParams, "Missing uri",
                     std::move(callback));
    return;
  }

  if (*uri != "abp://guide") {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Unknown resource: " + *uri, std::move(callback));
    return;
  }

  base::Value::Dict result;
  base::Value::List contents;

  base::Value::Dict content;
  content.Set("uri", "abp://guide");
  content.Set("mimeType", "text/markdown");
  content.Set("text", kGuideContent);
  contents.Append(std::move(content));

  result.Set("contents", std::move(contents));
  SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                    std::move(callback));
}

// --- 1. browser_action: batched input via /batch endpoint ---
void AbpMcpHandler::CallBrowserAction(const base::Value::Dict& args,
                                      base::Value request_id,
                                      ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward everything except tab_id to the batch REST endpoint.
  // The body includes "actions" array and optional "screenshot" config.
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  // Inject network_tag as {"network": {"tag": "..."}} if present.
  if (const std::string* network_tag = args.FindString("network_tag")) {
    body_dict.Remove("network_tag");
    base::Value::Dict network_dict;
    network_dict.Set("tag", *network_tag);
    body_dict.Set("network", std::move(network_dict));
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/batch", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 2. browser_scroll: standalone scroll ---
void AbpMcpHandler::CallBrowserScroll(const base::Value::Dict& args,
                                      base::Value request_id,
                                      ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  if (const std::string* network_tag = args.FindString("network_tag")) {
    body_dict.Remove("network_tag");
    base::Value::Dict network_dict;
    network_dict.Set("tag", *network_tag);
    body_dict.Set("network", std::move(network_dict));
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/scroll", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 3. browser_navigate: url or back/forward/reload ---
void AbpMcpHandler::CallBrowserNavigate(const base::Value::Dict& args,
                                        base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  const std::string* url = args.FindString("url");
  const std::string* action = args.FindString("action");
  const std::string* network_tag = args.FindString("network_tag");

  if (url) {
    base::Value::Dict body_dict;
    body_dict.Set("url", *url);
    if (network_tag) {
      base::Value::Dict network_dict;
      network_dict.Set("tag", *network_tag);
      body_dict.Set("network", std::move(network_dict));
    }
    std::string body;
    base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);
    controller_->HandleRequest(
        "POST", "/api/v1/tabs/" + tab_id + "/navigate", body,
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (action) {
    // Build optional network body for back/forward/reload
    std::string nav_action_body;
    if (network_tag) {
      base::Value::Dict nav_body;
      base::Value::Dict network_dict;
      network_dict.Set("tag", *network_tag);
      nav_body.Set("network", std::move(network_dict));
      base::JSONWriter::Write(base::Value(std::move(nav_body)),
                              &nav_action_body);
    }

    if (*action == "back") {
      controller_->HandleRequest(
          "POST", "/api/v1/tabs/" + tab_id + "/back", nav_action_body,
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else if (*action == "forward") {
      controller_->HandleRequest(
          "POST", "/api/v1/tabs/" + tab_id + "/forward", nav_action_body,
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else if (*action == "reload") {
      controller_->HandleRequest(
          "POST", "/api/v1/tabs/" + tab_id + "/reload", nav_action_body,
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "Invalid action: " + *action +
                           " (expected back, forward, or reload)",
                       std::move(callback));
    }
  } else {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Provide 'url' or 'action' (back/forward/reload)",
                     std::move(callback));
  }
}

// --- 4. browser_screenshot ---
void AbpMcpHandler::CallBrowserScreenshot(const base::Value::Dict& args,
                                          base::Value request_id,
                                          ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Build screenshot options from flat args.
  // network_tag is explicitly extracted below and converted to {"network":
  // {"tag": "..."}} — it is never cloned into body_dict, so no Remove() needed.
  base::Value::Dict body_dict;
  base::Value::Dict screenshot_opts;
  if (const base::Value::List* markup = args.FindList("markup")) {
    screenshot_opts.Set("markup", markup->Clone());
  } else if (const base::Value::List* disable_markup =
                 args.FindList("disable_markup")) {
    screenshot_opts.Set("disable_markup", disable_markup->Clone());
  }
  if (const std::string* format = args.FindString("format")) {
    screenshot_opts.Set("format", *format);
  }
  body_dict.Set("screenshot", std::move(screenshot_opts));

  if (const std::string* network_tag = args.FindString("network_tag")) {
    base::Value::Dict network_dict;
    network_dict.Set("tag", *network_tag);
    body_dict.Set("network", std::move(network_dict));
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/screenshot", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 4b. browser_wait: wait for network to settle ---
void AbpMcpHandler::CallBrowserWait(const base::Value::Dict& args,
                                     base::Value request_id,
                                     ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Build body with screenshot options (same shape as /screenshot).
  // network_tag is explicitly extracted below and converted to {"network":
  // {"tag": "..."}} — it is never cloned into body_dict, so no Remove() needed.
  base::Value::Dict body_dict;
  base::Value::Dict screenshot_opts;
  if (const base::Value::List* markup = args.FindList("markup")) {
    screenshot_opts.Set("markup", markup->Clone());
  }
  if (const std::string* format = args.FindString("format")) {
    screenshot_opts.Set("format", *format);
  }
  body_dict.Set("screenshot", std::move(screenshot_opts));

  if (args.FindBool("animation").value_or(false)) {
    body_dict.Set("animation", true);
  }

  if (const std::string* network_tag = args.FindString("network_tag")) {
    base::Value::Dict network_dict;
    network_dict.Set("tag", *network_tag);
    body_dict.Set("network", std::move(network_dict));
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/wait_for_network", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 5. browser_tabs: list/new/close/info/activate/stop ---
void AbpMcpHandler::CallBrowserTabs(const base::Value::Dict& args,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  const std::string* action = args.FindString("action");
  std::string act = action ? *action : "list";

  if (act == "list") {
    controller_->HandleRequest(
        "GET", "/api/v1/tabs", "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (act == "new") {
    // Reuse the existing new-tab-then-navigate logic.
    const std::string* url = args.FindString("url");
    std::string nav_url = url ? *url : "";

    // Always create tab at about:blank first.  If a URL was requested we
    // follow up with a navigate call so it goes through AbpActionContext
    // (which captures before/after screenshots and waits for page load).
    controller_->HandleRequest(
        "POST", "/api/v1/tabs", R"({"url":"about:blank"})",
        base::BindOnce(
            [](base::WeakPtr<AbpMcpHandler> self, std::string nav_url,
               base::Value request_id, ResponseWithHeadersCallback callback,
               int status, const std::string& content_type,
               std::string body) {
              if (!self)
                return;

              if (nav_url.empty() || nav_url == "about:blank") {
                self->OnControllerResponse(std::move(request_id),
                                           std::move(callback), status,
                                           content_type, std::move(body));
                return;
              }

              auto parsed =
                  base::JSONReader::Read(body, base::JSON_PARSE_RFC);
              std::string tab_id;
              if (parsed && parsed->is_dict()) {
                const std::string* id = parsed->GetDict().FindString("id");
                if (id)
                  tab_id = *id;
              }
              if (tab_id.empty()) {
                self->OnControllerResponse(std::move(request_id),
                                           std::move(callback), status,
                                           content_type, std::move(body));
                return;
              }

              VLOG(1) << "ABP MCP: browser_tabs new - tab created: "
                      << tab_id << ", scheduling navigate to " << nav_url
                      << " in 500ms";
              base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
                  FROM_HERE,
                  base::BindOnce(
                      [](base::WeakPtr<AbpMcpHandler> handler,
                         std::string tid, std::string url,
                         base::Value req_id,
                         ResponseWithHeadersCallback cb) {
                        if (!handler)
                          return;
                        base::Value::Dict nav_body;
                        nav_body.Set("url", url);
                        std::string nav_body_str;
                        base::JSONWriter::Write(
                            base::Value(std::move(nav_body)), &nav_body_str);
                        handler->controller_->HandleRequest(
                            "POST", "/api/v1/tabs/" + tid + "/navigate",
                            nav_body_str,
                            base::BindOnce(
                                &AbpMcpHandler::OnControllerResponse,
                                handler, std::move(req_id), std::move(cb)));
                      },
                      self, std::move(tab_id), std::move(nav_url),
                      std::move(request_id), std::move(callback)),
                  base::Milliseconds(500));
            },
            weak_factory_.GetWeakPtr(), std::move(nav_url),
            std::move(request_id), std::move(callback)));
  } else {
    // close, info, activate, stop all need a tab_id
    std::string tab_id = ResolveTabId(args);
    if (tab_id.empty()) {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "No tab_id provided and no active tab available",
                       std::move(callback));
      return;
    }

    if (act == "close") {
      controller_->HandleRequest(
          "DELETE", "/api/v1/tabs/" + tab_id, "",
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else if (act == "info") {
      controller_->HandleRequest(
          "GET", "/api/v1/tabs/" + tab_id, "",
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else if (act == "activate") {
      controller_->HandleRequest(
          "POST", "/api/v1/tabs/" + tab_id + "/activate", "",
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else if (act == "stop") {
      controller_->HandleRequest(
          "POST", "/api/v1/tabs/" + tab_id + "/stop", "",
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "Invalid action: " + act +
                           " (expected list, new, close, info, activate, "
                           "or stop)",
                       std::move(callback));
    }
  }
}

// --- 6. browser_javascript ---
void AbpMcpHandler::CallBrowserJavascript(const base::Value::Dict& args,
                                          base::Value request_id,
                                          ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Map MCP "expression" to REST "script".
  // network_tag is explicitly extracted below and converted to {"network":
  // {"tag": "..."}} — it is never cloned into body_dict, so no Remove() needed.
  base::Value::Dict body_dict;
  if (const std::string* expression = args.FindString("expression")) {
    body_dict.Set("script", *expression);
  }
  if (const std::string* network_tag = args.FindString("network_tag")) {
    base::Value::Dict network_dict;
    network_dict.Set("tag", *network_tag);
    body_dict.Set("network", std::move(network_dict));
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/execute", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 7. browser_text ---
void AbpMcpHandler::CallBrowserText(const base::Value::Dict& args,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // network_tag is explicitly extracted below and converted to {"network":
  // {"tag": "..."}} — it is never cloned into body_dict, so no Remove() needed.
  base::Value::Dict body_dict;
  if (const std::string* selector = args.FindString("selector")) {
    body_dict.Set("selector", *selector);
  }
  if (const std::string* network_tag = args.FindString("network_tag")) {
    base::Value::Dict network_dict;
    network_dict.Set("tag", *network_tag);
    body_dict.Set("network", std::move(network_dict));
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/text", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 8. browser_dialog: check/accept/dismiss ---
void AbpMcpHandler::CallBrowserDialog(const base::Value::Dict& args,
                                      base::Value request_id,
                                      ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  const std::string* action = args.FindString("action");
  std::string act = action ? *action : "check";

  if (act == "check") {
    controller_->HandleRequest(
        "GET", "/api/v1/tabs/" + tab_id + "/dialog", "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (act == "accept") {
    base::Value::Dict body_dict;
    if (const std::string* prompt_text = args.FindString("prompt_text")) {
      body_dict.Set("prompt_text", *prompt_text);
    }
    std::string body;
    base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);
    controller_->HandleRequest(
        "POST", "/api/v1/tabs/" + tab_id + "/dialog/accept", body,
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (act == "dismiss") {
    controller_->HandleRequest(
        "POST", "/api/v1/tabs/" + tab_id + "/dialog/dismiss", "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Invalid action: " + act +
                         " (expected check, accept, or dismiss)",
                     std::move(callback));
  }
}

// --- 9. browser_downloads: list/status/cancel ---
void AbpMcpHandler::CallBrowserDownloads(const base::Value::Dict& args,
                                         base::Value request_id,
                                         ResponseWithHeadersCallback callback) {
  const std::string* action = args.FindString("action");
  std::string act = action ? *action : "list";

  if (act == "list") {
    // Build query string from optional params
    std::string path = "/api/v1/downloads";
    std::vector<std::string> query_parts;

    if (const std::string* state = args.FindString("state")) {
      query_parts.push_back("state=" + *state);
    }
    if (auto limit = args.FindDouble("limit")) {
      query_parts.push_back(
          "limit=" + base::NumberToString(static_cast<int>(*limit)));
    }

    if (!query_parts.empty()) {
      path += "?";
      for (size_t i = 0; i < query_parts.size(); ++i) {
        if (i > 0)
          path += "&";
        path += query_parts[i];
      }
    }

    controller_->HandleRequest(
        "GET", path, "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (act == "status") {
    const std::string* download_id = args.FindString("download_id");
    if (!download_id) {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "Missing download_id for status action",
                       std::move(callback));
      return;
    }
    controller_->HandleRequest(
        "GET", "/api/v1/downloads/" + *download_id, "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (act == "cancel") {
    const std::string* download_id = args.FindString("download_id");
    if (!download_id) {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "Missing download_id for cancel action",
                       std::move(callback));
      return;
    }
    controller_->HandleRequest(
        "POST", "/api/v1/downloads/" + *download_id + "/cancel", "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (act == "content") {
    const std::string* download_id = args.FindString("download_id");
    if (!download_id) {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "Missing download_id for content action",
                       std::move(callback));
      return;
    }

    // Build query string for max_size
    std::string content_path =
        "/api/v1/downloads/" + *download_id + "/content";
    if (auto max_size = args.FindDouble("max_size")) {
      content_path += "?max_size=" +
                      base::NumberToString(static_cast<int64_t>(*max_size));
    }

    std::string dl_id = *download_id;

    // Fetch metadata first to get filename, then fetch content
    controller_->HandleRequest(
        "GET", "/api/v1/downloads/" + dl_id, "",
        base::BindOnce(
            [](base::WeakPtr<AbpMcpHandler> self, base::Value request_id,
               ResponseWithHeadersCallback callback, std::string dl_id,
               std::string content_path, int status,
               const std::string& content_type, std::string body) {
              if (!self)
                return;

              // Extract filename from metadata response
              std::string filename;
              auto parsed =
                  base::JSONReader::Read(body, base::JSON_PARSE_RFC);
              if (parsed && parsed->is_dict()) {
                const std::string* fn =
                    parsed->GetDict().FindString("filename");
                if (fn)
                  filename = *fn;
              }

              // Now fetch the actual content
              self->controller_->HandleRequest(
                  "GET", content_path, "",
                  base::BindOnce(&AbpMcpHandler::OnBinaryControllerResponse,
                                 self, std::move(request_id),
                                 std::move(dl_id), std::move(filename),
                                 std::move(callback)));
            },
            weak_factory_.GetWeakPtr(), std::move(request_id),
            std::move(callback), dl_id, content_path));
  } else {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Invalid action: " + act +
                         " (expected list, status, cancel, or content)",
                     std::move(callback));
  }
}

// --- 10. browser_files ---
void AbpMcpHandler::CallBrowserFiles(const base::Value::Dict& args,
                                     base::Value request_id,
                                     ResponseWithHeadersCallback callback) {
  const std::string* chooser_id = args.FindString("chooser_id");
  if (!chooser_id) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing chooser_id", std::move(callback));
    return;
  }

  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("chooser_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/file-chooser/" + *chooser_id, body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 11. browser_select_picker ---
void AbpMcpHandler::CallBrowserSelectPicker(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  const std::string* popup_id = args.FindString("popup_id");
  if (!popup_id) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing popup_id", std::move(callback));
    return;
  }

  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("popup_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/select/" + *popup_id, body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 11b. browser_datetime_picker ---
void AbpMcpHandler::CallBrowserDateTimePicker(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  const std::string* popup_id = args.FindString("popup_id");
  if (!popup_id) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing popup_id", std::move(callback));
    return;
  }

  // Translate the action discriminator to the REST cancel/value contract.
  const std::string* action = args.FindString("action");
  base::Value::Dict body_dict;
  if (action && *action == "cancel") {
    body_dict.Set("cancel", true);
  } else if (const std::string* value = args.FindString("value")) {
    body_dict.Set("value", *value);
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/datetime-picker/" + *popup_id, body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 12. browser_get_status ---
void AbpMcpHandler::CallBrowserGetStatus(const base::Value::Dict& args,
                                         base::Value request_id,
                                         ResponseWithHeadersCallback callback) {
  controller_->GetBrowserStatus(
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 14. browser_shutdown ---
void AbpMcpHandler::CallBrowserShutdown(const base::Value::Dict& args,
                                        base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  std::string body;
  base::JSONWriter::Write(base::Value(args.Clone()), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/browser/shutdown", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 15. browser_slider: standalone slider macro ---
void AbpMcpHandler::CallBrowserSlider(const base::Value::Dict& args,
                                      base::Value request_id,
                                      ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  if (const std::string* network_tag = args.FindString("network_tag")) {
    body_dict.Remove("network_tag");
    base::Value::Dict network_dict;
    network_dict.Set("tag", *network_tag);
    body_dict.Set("network", std::move(network_dict));
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/slider", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 16. browser_clear_text: clear input via backspace ---
void AbpMcpHandler::CallBrowserClearText(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  if (const std::string* network_tag = args.FindString("network_tag")) {
    body_dict.Remove("network_tag");
    base::Value::Dict network_dict;
    network_dict.Set("tag", *network_tag);
    body_dict.Set("network", std::move(network_dict));
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/clear_text", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 18. browser_network: query/save/clear network captures ---
void AbpMcpHandler::CallBrowserNetwork(const base::Value::Dict& args,
                                       base::Value request_id,
                                       ResponseWithHeadersCallback callback) {
  const std::string* action = args.FindString("action");
  if (!action) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing action (query, save, or clear)",
                     std::move(callback));
    return;
  }

  if (*action == "query") {
    // Build query string from optional filter params
    std::string path = "/api/v1/network";
    std::vector<std::string> qp;

    // URL-encode param values: regex filters may contain +, ?, (, ), |, & etc.
    auto add_str = [&](const char* param_name) {
      if (const std::string* v = args.FindString(param_name)) {
        qp.push_back(std::string(param_name) + "=" +
                     base::EscapeQueryParamValue(*v, /*use_plus=*/false));
      }
    };

    add_str("tag");
    add_str("tab_id");
    add_str("url");
    add_str("hostname");
    add_str("path");
    add_str("query");
    add_str("method");
    add_str("status");
    add_str("type");
    add_str("action_id");

    if (auto include_body = args.FindBool("include_body")) {
      if (*include_body) {
        qp.push_back("include_body=true");
      }
    }
    if (auto max_body = args.FindInt("max_body_size")) {
      qp.push_back("max_body_size=" + base::NumberToString(*max_body));
    }

    if (!qp.empty()) {
      path += "?";
      for (size_t i = 0; i < qp.size(); ++i) {
        if (i > 0)
          path += "&";
        path += qp[i];
      }
    }

    controller_->HandleRequest(
        "GET", path, "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));

  } else if (*action == "save") {
    // POST /api/v1/network/save with optional tag + tab_id
    base::Value::Dict body_dict;
    if (const std::string* tag = args.FindString("tag")) {
      body_dict.Set("tag", *tag);
    }
    if (const std::string* tab_id = args.FindString("tab_id")) {
      body_dict.Set("tab_id", *tab_id);
    }
    std::string body;
    base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

    controller_->HandleRequest(
        "POST", "/api/v1/network/save", body,
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));

  } else if (*action == "clear") {
    // DELETE /api/v1/network?tag=...
    std::string path = "/api/v1/network";
    if (const std::string* tag = args.FindString("tag")) {
      path += "?tag=" + base::EscapeQueryParamValue(*tag, /*use_plus=*/false);
    }

    controller_->HandleRequest(
        "DELETE", path, "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));

  } else {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Invalid action: " + *action +
                         " (expected query, save, or clear)",
                     std::move(callback));
  }
}

// --- 19. browser_curl: session-aware HTTP client ---
void AbpMcpHandler::CallBrowserCurl(const base::Value::Dict& args,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  const std::string* tab_id = args.FindString("tab_id");
  if (!tab_id || tab_id->empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing required tab_id", std::move(callback));
    return;
  }

  const std::string* url = args.FindString("url");
  if (!url || url->empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing required url", std::move(callback));
    return;
  }

  base::Value::Dict body_dict;
  body_dict.Set("url", *url);

  if (const std::string* method = args.FindString("method")) {
    body_dict.Set("method", *method);
  }
  if (const std::string* body_str = args.FindString("body")) {
    body_dict.Set("body", *body_str);
  }
  if (const base::Value::Dict* headers_dict = args.FindDict("headers")) {
    body_dict.Set("headers", headers_dict->Clone());
  }
  if (const std::string* tag = args.FindString("tag")) {
    body_dict.Set("tag", *tag);
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + *tab_id + "/curl", body,
      base::BindOnce(&AbpMcpHandler::OnCurlControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::OnCurlControllerResponse(base::Value request_id,
                                             ResponseWithHeadersCallback callback,
                                             int status,
                                             const std::string& content_type,
                                             std::string body) {
  // Parse the JSON response from the curl REST endpoint.
  // Expected shape:
  //   { "status": 200, "headers": {...}, "body": "...",
  //     "body_encoding": "text"|"base64", "content_type": "..." }
  //
  // For image responses (body_encoding == "base64" and content_type image/*),
  // emit a native MCP image content block so the model can view it directly.
  // For all other responses, forward through the normal text path.

  auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    // Not JSON — forward as plain text
    OnControllerResponse(std::move(request_id), std::move(callback), status,
                         content_type, std::move(body));
    return;
  }

  base::Value::Dict& resp = parsed->GetDict();

  const std::string* body_encoding = resp.FindString("body_encoding");
  const std::string* resp_content_type = resp.FindString("content_type");
  const std::string* resp_body_ptr = resp.FindString("body");

  bool is_base64 = body_encoding && *body_encoding == "base64";
  bool is_image = resp_content_type &&
                  resp_content_type->find("image/") == 0;

  if (is_base64 && is_image && resp_body_ptr && !resp_body_ptr->empty()) {
    // Return as an MCP image content block so the model can view it directly.

    // Capture body data and content type before mutating resp.
    std::string img_data = *resp_body_ptr;
    std::string img_mime = *resp_content_type;

    base::Value::List content;

    // Text block with metadata (status, headers, content_type) — strip body.
    resp.Remove("body");
    std::string meta_json;
    base::JSONWriter::WriteWithOptions(
        *parsed, base::JSONWriter::OPTIONS_PRETTY_PRINT, &meta_json);
    base::Value::Dict text_block;
    text_block.Set("type", "text");
    text_block.Set("text", meta_json);
    content.Append(std::move(text_block));

    // Image content block
    base::Value::Dict img_block;
    img_block.Set("type", "image");
    img_block.Set("data", std::move(img_data));
    img_block.Set("mimeType", std::move(img_mime));
    content.Append(std::move(img_block));

    base::Value::Dict result;
    result.Set("content", std::move(content));
    if (status >= 400) {
      result.Set("isError", true);
    }

    SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                      std::move(callback));
  } else {
    // Text or other binary — use the normal response handler
    OnControllerResponse(std::move(request_id), std::move(callback), status,
                         content_type, std::move(body));
  }
}

void AbpMcpHandler::CallBrowserConsole(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  // Check if this is a clear operation.
  if (auto clear = args.FindBool("clear"); clear && *clear) {
    std::string path = "/api/v1/console";
    if (const std::string* tab_id = args.FindString("tab_id")) {
      path += "?tab_id=" +
              base::EscapeQueryParamValue(*tab_id, /*use_plus=*/false);
    }
    controller_->HandleRequest(
        "DELETE", path, "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
    return;
  }

  // Build query string from optional filter params.
  std::string path = "/api/v1/console";
  std::vector<std::string> qp;

  if (const std::string* level = args.FindString("level")) {
    qp.push_back("level=" +
                 base::EscapeQueryParamValue(*level, /*use_plus=*/false));
  }
  if (const std::string* pattern = args.FindString("pattern")) {
    qp.push_back("pattern=" +
                 base::EscapeQueryParamValue(*pattern, /*use_plus=*/false));
  }
  if (const std::string* tab_id = args.FindString("tab_id")) {
    qp.push_back("tab_id=" +
                 base::EscapeQueryParamValue(*tab_id, /*use_plus=*/false));
  }
  if (auto limit = args.FindDouble("limit")) {
    qp.push_back("limit=" + base::NumberToString(static_cast<int>(*limit)));
  }
  if (auto after_id = args.FindDouble("after_id")) {
    qp.push_back("after_id=" +
                 base::NumberToString(static_cast<int64_t>(*after_id)));
  }

  if (!qp.empty()) {
    path += "?";
    for (size_t i = 0; i < qp.size(); ++i) {
      if (i > 0) path += "&";
      path += qp[i];
    }
  }

  controller_->HandleRequest(
      "GET", path, "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::OnControllerResponse(base::Value request_id,
                                         ResponseWithHeadersCallback callback,
                                         int status,
                                         const std::string& content_type,
                                         std::string body) {
  // Convert REST API response to MCP tool result.
  // Extract screenshot data into native MCP image content blocks so the
  // model can see pages directly, rather than embedding base64 in JSON text.

  base::Value::Dict result;
  base::Value::List content;

  auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (parsed && parsed->is_dict()) {
    base::Value::Dict& response_dict = parsed->GetDict();

    // Extract screenshot image data if present.
    // REST responses may contain screenshot_before + screenshot_after (action
    // envelope) or a single "screenshot" dict (legacy/screenshot endpoint).
    // For MCP: strip screenshot_before entirely, rename screenshot_after to
    // "screenshot", and emit the image as a native MCP image content block.

    // Helper to extract image data from a screenshot dict and determine mime
    auto extract_image = [](base::Value::Dict* dict, std::string& out_data,
                            std::string& out_mime) {
      if (!dict) return;
      std::string* data = dict->FindString("data");
      if (!data || data->empty()) return;
      out_data = std::move(*data);
      dict->Remove("data");
      out_mime = "image/webp";
      const std::string* format = dict->FindString("format");
      if (format) {
        if (*format == "png") out_mime = "image/png";
        else if (*format == "jpeg") out_mime = "image/jpeg";
      }
    };

    std::string after_data, after_mime;
    std::string single_data, single_mime;

    // Strip before screenshot entirely from MCP response (not useful to agents)
    response_dict.Remove("screenshot_before");

    // Extract intermediate screenshots from result.intermediate_screenshots
    // (multi-scroll actions). Strip data from JSON; emit as image blocks.
    std::vector<std::pair<std::string, std::string>> intermediate_images;
    if (base::Value::Dict* result_dict = response_dict.FindDict("result")) {
      if (base::Value::List* intermed =
              result_dict->FindList("intermediate_screenshots")) {
        for (auto& item : *intermed) {
          if (!item.is_dict()) continue;
          std::string img_data, img_mime;
          extract_image(&item.GetDict(), img_data, img_mime);
          if (!img_data.empty()) {
            intermediate_images.emplace_back(std::move(img_data), img_mime);
          }
        }
      }
      result_dict->Remove("intermediate_screenshots");
    }

    // Check new after format and rename to "screenshot" for MCP output
    extract_image(response_dict.FindDict("screenshot_after"),
                  after_data, after_mime);
    if (auto after_val = response_dict.Extract("screenshot_after")) {
      response_dict.Set("screenshot", std::move(*after_val));
    }

    // Fallback: legacy single "screenshot" dict
    if (after_data.empty()) {
      extract_image(response_dict.FindDict("screenshot"),
                    single_data, single_mime);
    }

    // Fallback: direct screenshot endpoint format
    if (after_data.empty() && single_data.empty()) {
      std::string* data = response_dict.FindString("data");
      const std::string* top_mime = response_dict.FindString("mimeType");
      if (data && !data->empty()) {
        single_data = std::move(*data);
        response_dict.Remove("data");
        single_mime = top_mime ? *top_mime : "image/webp";
      }
    }

    // Serialize the remaining JSON (without the large base64 data) as text
    std::string pretty_json;
    base::JSONWriter::WriteWithOptions(
        *parsed, base::JSONWriter::OPTIONS_PRETTY_PRINT, &pretty_json);
    base::Value::Dict text_content;
    text_content.Set("type", "text");
    text_content.Set("text", pretty_json);
    content.Append(std::move(text_content));

    // Add intermediate screenshots as image blocks (multi-scroll).
    // These appear before the final screenshot_after image block.
    for (auto& [img_data, img_mime] : intermediate_images) {
      base::Value::Dict img;
      img.Set("type", "image");
      img.Set("data", std::move(img_data));
      img.Set("mimeType", img_mime);
      content.Append(std::move(img));
    }

    // Add final screenshot (screenshot_after or single).
    if (!after_data.empty()) {
      base::Value::Dict img;
      img.Set("type", "image");
      img.Set("data", std::move(after_data));
      img.Set("mimeType", after_mime);
      content.Append(std::move(img));
    }
    // Fallback: single screenshot image
    if (!single_data.empty()) {
      base::Value::Dict img;
      img.Set("type", "image");
      img.Set("data", std::move(single_data));
      img.Set("mimeType", single_mime);
      content.Append(std::move(img));
    }
  } else {
    // Not JSON, return as-is
    base::Value::Dict text_content;
    text_content.Set("type", "text");
    text_content.Set("text", body);
    content.Append(std::move(text_content));
  }

  result.Set("content", std::move(content));

  // If the REST call failed, mark as error
  if (status >= 400) {
    result.Set("isError", true);
  }

  SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::OnBinaryControllerResponse(
    base::Value request_id,
    std::string download_id,
    std::string filename,
    ResponseWithHeadersCallback callback,
    int status,
    const std::string& content_type,
    std::string body) {
  if (status >= 400) {
    // Error response — likely JSON, forward through normal path
    OnControllerResponse(std::move(request_id), std::move(callback), status,
                         content_type, std::move(body));
    return;
  }

  // Base64-encode the binary content
  std::string base64_data = base::Base64Encode(body);

  // Build metadata text content
  base::Value::Dict metadata;
  metadata.Set("id", download_id);
  metadata.Set("filename", filename);
  metadata.Set("mime_type", content_type);
  metadata.Set("size", static_cast<int>(body.size()));

  std::string metadata_json;
  base::JSONWriter::Write(base::Value(std::move(metadata)), &metadata_json);

  base::Value::List content;

  // Text content with metadata
  base::Value::Dict text_block;
  text_block.Set("type", "text");
  text_block.Set("text", metadata_json);
  content.Append(std::move(text_block));

  // EmbeddedResource with BlobResourceContents
  base::Value::Dict resource_contents;
  resource_contents.Set("uri",
                        "download://" + download_id + "/" + filename);
  resource_contents.Set("mimeType",
                        content_type.empty() ? "application/octet-stream"
                                             : content_type);
  resource_contents.Set("blob", std::move(base64_data));

  base::Value::Dict resource_block;
  resource_block.Set("type", "resource");
  resource_block.Set("resource", std::move(resource_contents));
  content.Append(std::move(resource_block));

  base::Value::Dict result;
  result.Set("content", std::move(content));

  SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::SendJsonRpcResult(base::Value request_id,
                                      base::Value result,
                                      ResponseWithHeadersCallback callback) {
  base::Value::Dict response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", std::move(request_id));
  response.Set("result", std::move(result));

  std::string response_json;
  base::JSONWriter::Write(base::Value(std::move(response)), &response_json);

  std::move(callback).Run(200, "application/json", {}, response_json);
}

void AbpMcpHandler::SendJsonRpcError(base::Value request_id,
                                     int error_code,
                                     const std::string& message,
                                     ResponseWithHeadersCallback callback) {
  base::Value::Dict response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", std::move(request_id));

  base::Value::Dict error;
  error.Set("code", error_code);
  error.Set("message", message);
  response.Set("error", std::move(error));

  std::string response_json;
  base::JSONWriter::Write(base::Value(std::move(response)), &response_json);

  std::move(callback).Run(200, "application/json", {}, response_json);
}

void AbpMcpHandler::SendAccepted(ResponseWithHeadersCallback callback) {
  std::move(callback).Run(202, "application/json", {}, "");
}

// --- 16. respond_to_permission ---
void AbpMcpHandler::CallRespondToPermission(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  const std::string* perm_id = args.FindString("permission_id");
  if (!perm_id) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing permission_id", std::move(callback));
    return;
  }

  const std::string* perm_type = args.FindString("permission_type");
  if (!perm_type) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing permission_type", std::move(callback));
    return;
  }

  auto allow = args.FindBool("allow");
  if (!allow.has_value()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing allow boolean", std::move(callback));
    return;
  }

  std::string action = *allow ? "grant" : "deny";

  // Build body with permission_type and optional geolocation coordinates.
  base::Value::Dict body_dict;
  body_dict.Set("permission_type", *perm_type);
  if (auto latitude = args.FindDouble("latitude"))
    body_dict.Set("latitude", *latitude);
  if (auto longitude = args.FindDouble("longitude"))
    body_dict.Set("longitude", *longitude);
  if (auto accuracy = args.FindDouble("accuracy"))
    body_dict.Set("accuracy", *accuracy);

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/permissions/" + *perm_id + "/" + action, body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- cdp_mode ---
void AbpMcpHandler::CallCdpMode(const base::Value::Dict& args,
                                 base::Value request_id,
                                 ResponseWithHeadersCallback callback) {
  const std::string* action = args.FindString("action");
  if (!action) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing required parameter: action",
                     std::move(callback));
    return;
  }

  if (*action == "enter") {
    std::string body;
    base::JSONWriter::Write(base::Value(args.Clone()), &body);
    controller_->HandleRequest(
        "POST", "/api/v1/browser/cdp-mode/enter", body,
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (*action == "exit") {
    controller_->HandleRequest(
        "POST", "/api/v1/browser/cdp-mode/exit", "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Invalid action: must be 'enter' or 'exit'",
                     std::move(callback));
  }
}

}  // namespace abp
