#include "chrome/browser/abp/abp_history_controller.h"

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/browser_thread.h"

namespace abp {

namespace {

// Parse URL-encoded query string
std::map<std::string, std::string> ParseQueryString(const std::string& query) {
  std::map<std::string, std::string> result;

  for (const auto& param : base::SplitString(
           query, "&", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    size_t eq = param.find('=');
    if (eq != std::string::npos) {
      result[param.substr(0, eq)] = param.substr(eq + 1);
    } else {
      result[param] = "";
    }
  }

  return result;
}

// Read file content on ThreadPool
std::string ReadFileContent(const base::FilePath& path) {
  std::string content;
  base::ReadFileToString(path, &content);
  return content;
}

// Convert action record to JSON value
base::Value::Dict ActionRecordToJson(const ActionRecord& action) {
  base::Value::Dict dict;
  dict.Set("id", static_cast<int>(action.id));
  dict.Set("session_id", action.session_id);
  dict.Set("tab_id", action.tab_id);
  dict.Set("action_type", action.action_type);
  dict.Set("timestamp", static_cast<double>(action.timestamp));
  dict.Set("duration_ms", static_cast<int>(action.duration_ms));
  dict.Set("success", action.success);

  if (!action.params_json.empty()) {
    auto params = base::JSONReader::Read(action.params_json, base::JSON_PARSE_RFC);
    if (params) {
      dict.Set("params", std::move(*params));
    }
  }

  if (!action.result_json.empty()) {
    auto result = base::JSONReader::Read(action.result_json, base::JSON_PARSE_RFC);
    if (result) {
      dict.Set("result", std::move(*result));
    }
  }

  if (!action.success) {
    if (!action.error_code.empty()) {
      dict.Set("error_code", action.error_code);
    }
    if (!action.error_message.empty()) {
      dict.Set("error_message", action.error_message);
    }
  }

  if (!action.screenshot_before_path.empty()) {
    dict.Set("screenshot_before_path", action.screenshot_before_path);
  }
  if (!action.screenshot_after_path.empty()) {
    dict.Set("screenshot_after_path", action.screenshot_after_path);
  }

  return dict;
}

// Convert event record to JSON value
base::Value::Dict EventRecordToJson(const EventRecord& event) {
  base::Value::Dict dict;
  dict.Set("id", static_cast<int>(event.id));
  dict.Set("session_id", event.session_id);
  dict.Set("tab_id", event.tab_id);
  dict.Set("event_type", event.event_type);
  dict.Set("timestamp", static_cast<double>(event.timestamp));

  if (!event.data_json.empty()) {
    auto data = base::JSONReader::Read(event.data_json, base::JSON_PARSE_RFC);
    if (data) {
      dict.Set("data", std::move(*data));
    }
  }

  return dict;
}

// Convert session record to JSON value
base::Value::Dict SessionRecordToJson(const SessionRecord& session) {
  base::Value::Dict dict;
  dict.Set("id", session.id);
  dict.Set("start_time", static_cast<double>(session.start_time));
  if (session.end_time > 0) {
    dict.Set("end_time", static_cast<double>(session.end_time));
  } else {
    dict.Set("end_time", base::Value());
  }
  dict.Set("browser_version", session.browser_version);
  if (!session.user_agent.empty()) {
    dict.Set("user_agent", session.user_agent);
  }
  return dict;
}

}  // namespace

AbpHistoryController::AbpHistoryController(const AbpConfig& config)
    : config_(config), enabled_(config.history.enabled) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
}

AbpHistoryController::~AbpHistoryController() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
}

void AbpHistoryController::Initialize() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!enabled_) {
    LOG(INFO) << "ABP: History is disabled";
    return;
  }

  // Create database
  database_ =
      std::make_unique<AbpHistoryDatabase>(config_.history.database_path);

  database_->Initialize(base::BindOnce(
      [](base::WeakPtr<AbpHistoryController> self) {
        if (!self) {
          return;
        }

        // Create session
        self->session_id_ = GenerateSessionId();
        self->session_start_time_ =
            base::Time::Now().InMillisecondsSinceUnixEpoch();

        SessionRecord session;
        session.id = self->session_id_;
        session.start_time = self->session_start_time_;
        session.browser_version = version_info::GetVersionNumber();
        // User agent would require WebContents - skip for now

        self->database_->InsertSession(session, base::DoNothing());

        LOG(INFO) << "ABP: History session started: " << self->session_id_;
      },
      weak_factory_.GetWeakPtr()));

  // Ensure screenshots directory exists
  if (config_.history.screenshots.enabled) {
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            [](base::FilePath dir) {
              if (!base::DirectoryExists(dir)) {
                base::CreateDirectory(dir);
              }
            },
            config_.history.screenshots.directory));
  }
}

void AbpHistoryController::Shutdown() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!enabled_ || !database_) {
    return;
  }

  // Update session end time
  int64_t end_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  database_->UpdateSessionEndTime(session_id_, end_time, base::DoNothing());

  // Flush pending writes
  database_->FlushOnShutdown();

  LOG(INFO) << "ABP: History session ended: " << session_id_;
}

void AbpHistoryController::RecordAction(const std::string& tab_id,
                                        const std::string& action_type,
                                        const base::Value::Dict& params,
                                        const base::Value* result,
                                        bool success,
                                        const std::string& error_code,
                                        const std::string& error_message,
                                        int64_t start_time,
                                        int64_t duration_ms,
                                        const std::string& screenshot_before_path,
                                        const std::string& screenshot_after_path) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!enabled_ || !database_) {
    return;
  }

  ActionRecord action;
  action.session_id = session_id_;
  action.tab_id = tab_id;
  action.action_type = action_type;
  action.timestamp = start_time;
  action.duration_ms = duration_ms;
  action.success = success;
  action.error_code = error_code;
  action.error_message = error_message;
  action.screenshot_before_path = screenshot_before_path;
  action.screenshot_after_path = screenshot_after_path;

  // Serialize params and result to JSON
  base::JSONWriter::Write(params, &action.params_json);
  if (result) {
    base::JSONWriter::Write(*result, &action.result_json);
  }

  database_->InsertAction(action, base::DoNothing());
}

void AbpHistoryController::RecordEvent(const std::string& tab_id,
                                       const std::string& event_type,
                                       const base::Value::Dict& data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!enabled_ || !database_) {
    return;
  }

  EventRecord event;
  event.session_id = session_id_;
  event.tab_id = tab_id;
  event.event_type = event_type;
  event.timestamp = base::Time::Now().InMillisecondsSinceUnixEpoch();

  base::JSONWriter::Write(data, &event.data_json);

  database_->InsertEvent(event, base::DoNothing());
}

base::FilePath AbpHistoryController::GetScreenshotPath(const std::string& tab_id,
                                                       int64_t timestamp,
                                                       bool is_before) {
  if (!ScreenshotsEnabled()) {
    return base::FilePath();
  }

  std::string filename = base::NumberToString(timestamp) + "_" + tab_id + "_" +
                         (is_before ? "before" : "after") + ".webp";
  return config_.history.screenshots.directory.Append(filename);
}

void AbpHistoryController::HandleRequest(const std::string& method,
                                         const std::string& path,
                                         const std::string& body,
                                         ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!enabled_) {
    SendError(503, "HISTORY_DISABLED", "History recording is disabled",
              std::move(callback));
    return;
  }

  // Parse path: /api/v1/history/...
  // Extract query string if present
  std::string clean_path = path;
  std::string query;
  size_t query_pos = path.find('?');
  if (query_pos != std::string::npos) {
    clean_path = path.substr(0, query_pos);
    query = path.substr(query_pos + 1);
  }

  // Split path into segments
  std::vector<std::string> segments;
  for (const auto& segment : base::SplitString(
           clean_path, "/", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    segments.push_back(segment);
  }

  // Expect at least: api/v1/history
  if (segments.size() < 3 || segments[0] != "api" || segments[1] != "v1" ||
      segments[2] != "history") {
    SendError(404, "NOT_FOUND", "Not found", std::move(callback));
    return;
  }

  // /api/v1/history (bulk delete)
  if (segments.size() == 3) {
    if (method == "DELETE") {
      HandleDeleteAll(query, std::move(callback));
    } else {
      SendError(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                std::move(callback));
    }
    return;
  }

  const std::string& resource = segments[3];

  // /api/v1/history/sessions
  if (resource == "sessions") {
    if (segments.size() == 4) {
      if (method == "GET") {
        HandleGetSessions(query, std::move(callback));
      } else {
        SendError(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                  std::move(callback));
      }
      return;
    }

    if (segments.size() >= 5) {
      const std::string& session_id = segments[4];

      if (session_id == "current") {
        if (method == "GET") {
          HandleGetCurrentSession(std::move(callback));
        } else {
          SendError(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                    std::move(callback));
        }
        return;
      }

      if (segments.size() == 5) {
        if (method == "GET") {
          HandleGetSession(session_id, std::move(callback));
        } else {
          SendError(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                    std::move(callback));
        }
        return;
      }

      if (segments.size() == 6 && segments[5] == "export") {
        if (method == "GET") {
          HandleExportSession(session_id, query, std::move(callback));
        } else {
          SendError(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                    std::move(callback));
        }
        return;
      }
    }
  }

  // /api/v1/history/actions
  if (resource == "actions") {
    if (segments.size() == 4) {
      if (method == "GET") {
        HandleGetActions(query, std::move(callback));
      } else if (method == "DELETE") {
        HandleDeleteActions(query, std::move(callback));
      } else {
        SendError(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                  std::move(callback));
      }
      return;
    }

    if (segments.size() >= 5) {
      int64_t action_id = 0;
      if (!base::StringToInt64(segments[4], &action_id)) {
        SendError(400, "INVALID_QUERY", "Invalid action ID",
                  std::move(callback));
        return;
      }

      if (segments.size() == 5) {
        if (method == "GET") {
          HandleGetAction(action_id, std::move(callback));
        } else {
          SendError(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                    std::move(callback));
        }
        return;
      }

      if (segments.size() == 6 && segments[5] == "screenshot") {
        if (method == "GET") {
          auto params = ParseQueryString(query);
          std::string type = "after";
          auto it = params.find("type");
          if (it != params.end()) {
            type = it->second;
          }
          HandleGetActionScreenshot(action_id, type, std::move(callback));
        } else {
          SendError(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                    std::move(callback));
        }
        return;
      }
    }
  }

  // /api/v1/history/events
  if (resource == "events") {
    if (segments.size() == 4) {
      if (method == "GET") {
        HandleGetEvents(query, std::move(callback));
      } else if (method == "DELETE") {
        HandleDeleteEvents(query, std::move(callback));
      } else {
        SendError(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                  std::move(callback));
      }
      return;
    }

    if (segments.size() == 5) {
      int64_t event_id = 0;
      if (!base::StringToInt64(segments[4], &event_id)) {
        SendError(400, "INVALID_QUERY", "Invalid event ID",
                  std::move(callback));
        return;
      }

      if (method == "GET") {
        HandleGetEvent(event_id, std::move(callback));
      } else {
        SendError(405, "METHOD_NOT_ALLOWED", "Method not allowed",
                  std::move(callback));
      }
      return;
    }
  }

  SendError(404, "NOT_FOUND", "Not found", std::move(callback));
}

// Session handlers

void AbpHistoryController::HandleGetSessions(const std::string& query,
                                             ResponseCallback callback) {
  auto params = ParseQueryString(query);

  int limit = 20;
  if (params.count("limit")) {
    base::StringToInt(params["limit"], &limit);
    limit = std::clamp(limit, 1, 100);
  }

  std::string cursor = params.count("cursor") ? params["cursor"] : "";

  bool forward = true;
  if (params.count("direction") && params["direction"] == "backward") {
    forward = false;
  }

  bool active_only = false;
  if (params.count("active") && params["active"] == "true") {
    active_only = true;
  }

  database_->GetSessions(
      limit, cursor, forward, active_only,
      base::BindOnce(&AbpHistoryController::OnSessionsResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpHistoryController::HandleGetCurrentSession(
    ResponseCallback callback) {
  database_->GetSession(
      session_id_,
      base::BindOnce(&AbpHistoryController::OnSessionResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpHistoryController::HandleGetSession(const std::string& session_id,
                                            ResponseCallback callback) {
  database_->GetSession(
      session_id,
      base::BindOnce(&AbpHistoryController::OnSessionResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpHistoryController::OnSessionsResult(ResponseCallback callback,
                                            SessionsResult result) {
  base::Value::Dict response;
  response.Set("success", true);

  base::Value::Dict data;
  base::Value::List sessions;
  for (const auto& session : result.sessions) {
    sessions.Append(SessionRecordToJson(session));
  }
  data.Set("sessions", std::move(sessions));

  if (!result.prev_cursor.empty()) {
    data.Set("prev_cursor", result.prev_cursor);
  } else {
    data.Set("prev_cursor", base::Value());
  }
  if (!result.next_cursor.empty()) {
    data.Set("next_cursor", result.next_cursor);
  } else {
    data.Set("next_cursor", base::Value());
  }
  data.Set("has_more", result.has_more);

  response.Set("data", std::move(data));
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpHistoryController::OnSessionResult(
    ResponseCallback callback,
    std::optional<SessionRecord> session) {
  if (!session) {
    SendError(404, "SESSION_NOT_FOUND", "Session not found",
              std::move(callback));
    return;
  }

  base::Value::Dict response;
  response.Set("success", true);
  response.Set("data", SessionRecordToJson(*session));
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

// Action handlers

void AbpHistoryController::HandleGetActions(const std::string& query,
                                            ResponseCallback callback) {
  auto params = ParseQueryString(query);

  ActionQueryFilter filter;
  filter.session_id = params.count("session_id") ? params["session_id"] : session_id_;
  filter.tab_id = params.count("tab_id") ? params["tab_id"] : "";
  filter.action_type = params.count("action_type") ? params["action_type"] : "";

  if (params.count("success")) {
    filter.success = params["success"] == "true";
  }

  if (params.count("start_time")) {
    base::StringToInt64(params["start_time"], &filter.start_time);
  }
  if (params.count("end_time")) {
    base::StringToInt64(params["end_time"], &filter.end_time);
  }

  filter.limit = 50;
  if (params.count("limit")) {
    base::StringToInt(params["limit"], &filter.limit);
    filter.limit = std::clamp(filter.limit, 1, 500);
  }

  if (params.count("cursor")) {
    base::StringToInt64(params["cursor"], &filter.cursor);
  }

  filter.forward = false;
  if (params.count("direction") && params["direction"] == "forward") {
    filter.forward = true;
  }

  database_->GetActions(
      filter, base::BindOnce(&AbpHistoryController::OnActionsResult,
                             weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpHistoryController::HandleGetAction(int64_t action_id,
                                           ResponseCallback callback) {
  database_->GetAction(
      action_id, base::BindOnce(&AbpHistoryController::OnActionResult,
                                weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpHistoryController::HandleGetActionScreenshot(
    int64_t action_id,
    const std::string& type,
    ResponseCallback callback) {
  database_->GetAction(
      action_id, base::BindOnce(&AbpHistoryController::OnActionForScreenshot,
                                weak_factory_.GetWeakPtr(), std::move(callback),
                                type));
}

void AbpHistoryController::HandleDeleteActions(const std::string& query,
                                               ResponseCallback callback) {
  auto params = ParseQueryString(query);

  std::string session_id = params.count("session_id") ? params["session_id"] : "";
  std::string tab_id = params.count("tab_id") ? params["tab_id"] : "";
  int64_t before_time = 0;
  if (params.count("before")) {
    base::StringToInt64(params["before"], &before_time);
  }

  database_->DeleteActions(
      session_id, tab_id, before_time,
      base::BindOnce(&AbpHistoryController::OnDeleteResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpHistoryController::OnActionsResult(ResponseCallback callback,
                                           ActionsResult result) {
  base::Value::Dict response;
  response.Set("success", true);

  base::Value::Dict data;
  base::Value::List actions;
  for (const auto& action : result.actions) {
    actions.Append(ActionRecordToJson(action));
  }
  data.Set("actions", std::move(actions));

  if (!result.prev_cursor.empty()) {
    data.Set("prev_cursor", result.prev_cursor);
  } else {
    data.Set("prev_cursor", base::Value());
  }
  if (!result.next_cursor.empty()) {
    data.Set("next_cursor", result.next_cursor);
  } else {
    data.Set("next_cursor", base::Value());
  }
  data.Set("has_more", result.has_more);

  response.Set("data", std::move(data));
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpHistoryController::OnActionResult(
    ResponseCallback callback,
    std::optional<ActionRecord> action) {
  if (!action) {
    SendError(404, "ACTION_NOT_FOUND", "Action not found",
              std::move(callback));
    return;
  }

  base::Value::Dict response;
  response.Set("success", true);
  response.Set("data", ActionRecordToJson(*action));
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpHistoryController::OnActionForScreenshot(
    ResponseCallback callback,
    const std::string& type,
    std::optional<ActionRecord> action) {
  if (!action) {
    SendError(404, "ACTION_NOT_FOUND", "Action not found",
              std::move(callback));
    return;
  }

  std::string path_str;
  if (type == "before") {
    path_str = action->screenshot_before_path;
  } else {
    path_str = action->screenshot_after_path;
  }

  if (path_str.empty()) {
    SendError(404, "SCREENSHOT_NOT_FOUND",
              "Screenshot not available for this action", std::move(callback));
    return;
  }

  base::FilePath path(path_str);
  SendBinaryFile(path, "image/webp", std::move(callback));
}

// Event handlers

void AbpHistoryController::HandleGetEvents(const std::string& query,
                                           ResponseCallback callback) {
  auto params = ParseQueryString(query);

  EventQueryFilter filter;
  filter.session_id = params.count("session_id") ? params["session_id"] : session_id_;
  filter.tab_id = params.count("tab_id") ? params["tab_id"] : "";
  filter.event_type = params.count("event_type") ? params["event_type"] : "";

  if (params.count("start_time")) {
    base::StringToInt64(params["start_time"], &filter.start_time);
  }
  if (params.count("end_time")) {
    base::StringToInt64(params["end_time"], &filter.end_time);
  }

  filter.limit = 100;
  if (params.count("limit")) {
    base::StringToInt(params["limit"], &filter.limit);
    filter.limit = std::clamp(filter.limit, 1, 1000);
  }

  if (params.count("cursor")) {
    base::StringToInt64(params["cursor"], &filter.cursor);
  }

  filter.forward = false;
  if (params.count("direction") && params["direction"] == "forward") {
    filter.forward = true;
  }

  database_->GetEvents(
      filter, base::BindOnce(&AbpHistoryController::OnEventsResult,
                             weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpHistoryController::HandleGetEvent(int64_t event_id,
                                          ResponseCallback callback) {
  database_->GetEvent(
      event_id, base::BindOnce(&AbpHistoryController::OnEventResult,
                               weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpHistoryController::HandleDeleteEvents(const std::string& query,
                                              ResponseCallback callback) {
  auto params = ParseQueryString(query);

  std::string session_id = params.count("session_id") ? params["session_id"] : "";
  std::string tab_id = params.count("tab_id") ? params["tab_id"] : "";
  std::string event_type = params.count("event_type") ? params["event_type"] : "";
  int64_t before_time = 0;
  if (params.count("before")) {
    base::StringToInt64(params["before"], &before_time);
  }

  database_->DeleteEvents(
      session_id, tab_id, event_type, before_time,
      base::BindOnce(&AbpHistoryController::OnDeleteResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpHistoryController::OnEventsResult(ResponseCallback callback,
                                          EventsResult result) {
  base::Value::Dict response;
  response.Set("success", true);

  base::Value::Dict data;
  base::Value::List events;
  for (const auto& event : result.events) {
    events.Append(EventRecordToJson(event));
  }
  data.Set("events", std::move(events));

  if (!result.prev_cursor.empty()) {
    data.Set("prev_cursor", result.prev_cursor);
  } else {
    data.Set("prev_cursor", base::Value());
  }
  if (!result.next_cursor.empty()) {
    data.Set("next_cursor", result.next_cursor);
  } else {
    data.Set("next_cursor", base::Value());
  }
  data.Set("has_more", result.has_more);

  response.Set("data", std::move(data));
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpHistoryController::OnEventResult(
    ResponseCallback callback,
    std::optional<EventRecord> event) {
  if (!event) {
    SendError(404, "EVENT_NOT_FOUND", "Event not found", std::move(callback));
    return;
  }

  base::Value::Dict response;
  response.Set("success", true);
  response.Set("data", EventRecordToJson(*event));
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

// Delete handlers

void AbpHistoryController::HandleDeleteAll(const std::string& query,
                                           ResponseCallback callback) {
  auto params = ParseQueryString(query);

  bool confirm = params.count("confirm") && params["confirm"] == "true";
  if (!confirm) {
    SendError(400, "INVALID_QUERY",
              "Must pass confirm=true to delete all history",
              std::move(callback));
    return;
  }

  std::string session_id = params.count("session_id") ? params["session_id"] : "";

  database_->DeleteAll(
      session_id, confirm,
      base::BindOnce(&AbpHistoryController::OnBulkDeleteResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpHistoryController::OnDeleteResult(ResponseCallback callback,
                                          DeleteResult result) {
  base::Value::Dict response;
  response.Set("success", true);

  base::Value::Dict data;
  data.Set("deleted_count", static_cast<int>(result.deleted_count));
  response.Set("data", std::move(data));

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpHistoryController::OnBulkDeleteResult(ResponseCallback callback,
                                              BulkDeleteResult result) {
  base::Value::Dict response;
  response.Set("success", true);

  base::Value::Dict data;
  data.Set("deleted_sessions", static_cast<int>(result.deleted_sessions));
  data.Set("deleted_actions", static_cast<int>(result.deleted_actions));
  data.Set("deleted_events", static_cast<int>(result.deleted_events));
  response.Set("data", std::move(data));

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

// Export handlers

void AbpHistoryController::HandleExportSession(
    const std::string& session_id,
    const std::string& query,
    ResponseCallback callback) {
  database_->GetSession(
      session_id,
      base::BindOnce(&AbpHistoryController::OnExportSessionResult,
                     weak_factory_.GetWeakPtr(), std::move(callback), query));
}

void AbpHistoryController::OnExportSessionResult(
    ResponseCallback callback,
    const std::string& query,
    std::optional<SessionRecord> session) {
  if (!session) {
    SendError(404, "SESSION_NOT_FOUND", "Session not found",
              std::move(callback));
    return;
  }

  auto params = ParseQueryString(query);

  int chunk_size = 1000;
  if (params.count("chunk_size")) {
    base::StringToInt(params["chunk_size"], &chunk_size);
    chunk_size = std::clamp(chunk_size, 1, 5000);
  }

  bool include_screenshots = false;
  if (params.count("include_screenshots") &&
      params["include_screenshots"] == "true") {
    include_screenshots = true;
  }

  std::string actions_cursor =
      params.count("actions_cursor") ? params["actions_cursor"] : "";
  std::string events_cursor =
      params.count("events_cursor") ? params["events_cursor"] : "";

  // Get actions first
  ActionQueryFilter filter;
  filter.session_id = session->id;
  filter.limit = chunk_size;
  if (!actions_cursor.empty()) {
    base::StringToInt64(actions_cursor, &filter.cursor);
  }
  filter.forward = true;

  database_->GetActions(
      filter, base::BindOnce(&AbpHistoryController::OnExportActionsResult,
                             weak_factory_.GetWeakPtr(), std::move(callback),
                             *session, events_cursor, chunk_size,
                             include_screenshots));
}

void AbpHistoryController::OnExportActionsResult(
    ResponseCallback callback,
    SessionRecord session,
    const std::string& events_cursor,
    int chunk_size,
    bool include_screenshots,
    ActionsResult actions) {
  // Get events
  EventQueryFilter filter;
  filter.session_id = session.id;
  filter.limit = chunk_size;
  if (!events_cursor.empty()) {
    base::StringToInt64(events_cursor, &filter.cursor);
  }
  filter.forward = true;

  database_->GetEvents(
      filter, base::BindOnce(&AbpHistoryController::OnExportEventsResult,
                             weak_factory_.GetWeakPtr(), std::move(callback),
                             session, std::move(actions), include_screenshots));
}

void AbpHistoryController::OnExportEventsResult(
    ResponseCallback callback,
    SessionRecord session,
    ActionsResult actions,
    bool include_screenshots,
    EventsResult events) {
  base::Value::Dict response;
  response.Set("success", true);

  base::Value::Dict data;
  data.Set("session", SessionRecordToJson(session));

  // Convert actions
  base::Value::List actions_list;
  for (const auto& action : actions.actions) {
    base::Value::Dict action_dict = ActionRecordToJson(action);

    // Include base64 screenshots if requested
    if (include_screenshots) {
      if (!action.screenshot_before_path.empty()) {
        base::FilePath path(action.screenshot_before_path);
        std::string content;
        if (base::ReadFileToString(path, &content)) {
          action_dict.Set("screenshot_before", base::Base64Encode(content));
        }
      }
      if (!action.screenshot_after_path.empty()) {
        base::FilePath path(action.screenshot_after_path);
        std::string content;
        if (base::ReadFileToString(path, &content)) {
          action_dict.Set("screenshot_after", base::Base64Encode(content));
        }
      }
    }

    actions_list.Append(std::move(action_dict));
  }
  data.Set("actions", std::move(actions_list));

  // Convert events
  base::Value::List events_list;
  for (const auto& event : events.events) {
    events_list.Append(EventRecordToJson(event));
  }
  data.Set("events", std::move(events_list));

  // Cursors for continuation
  if (!actions.next_cursor.empty()) {
    data.Set("next_actions_cursor", actions.next_cursor);
  } else {
    data.Set("next_actions_cursor", base::Value());
  }
  if (!events.next_cursor.empty()) {
    data.Set("next_events_cursor", events.next_cursor);
  } else {
    data.Set("next_events_cursor", base::Value());
  }

  bool export_complete = !actions.has_more && !events.has_more;
  data.Set("export_complete", export_complete);

  response.Set("data", std::move(data));
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

// Helper methods

void AbpHistoryController::SendJson(int status,
                                    base::Value value,
                                    ResponseCallback callback) {
  std::string json;
  base::JSONWriter::Write(value, &json);
  std::move(callback).Run(status, "application/json", std::move(json));
}

void AbpHistoryController::SendError(int status,
                                     const std::string& error_code,
                                     const std::string& message,
                                     ResponseCallback callback) {
  base::Value::Dict response;
  response.Set("success", false);
  response.Set("error", error_code);
  response.Set("message", message);
  SendJson(status, base::Value(std::move(response)), std::move(callback));
}

void AbpHistoryController::SendBinaryFile(const base::FilePath& path,
                                          const std::string& content_type,
                                          ResponseCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&ReadFileContent, path),
      base::BindOnce(
          [](ResponseCallback cb, const std::string& content_type,
             std::string content) {
            if (content.empty()) {
              base::Value::Dict error;
              error.Set("success", false);
              error.Set("error", "SCREENSHOT_NOT_FOUND");
              error.Set("message", "Screenshot file not found");
              std::string json;
              base::JSONWriter::Write(error, &json);
              std::move(cb).Run(404, "application/json", std::move(json));
            } else {
              // Return raw binary content with appropriate content type
              std::move(cb).Run(200, content_type, std::move(content));
            }
          },
          std::move(callback), content_type));
}

// static
std::map<std::string, std::string> AbpHistoryController::ParseQuery(
    const std::string& query) {
  return ParseQueryString(query);
}

// static
std::string AbpHistoryController::GenerateSessionId() {
  return base::Uuid::GenerateRandomV4().AsLowercaseString();
}

}  // namespace abp
