#ifndef CHROME_BROWSER_ABP_ABP_HISTORY_CONTROLLER_H_
#define CHROME_BROWSER_ABP_ABP_HISTORY_CONTROLLER_H_

#include <map>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/abp/abp_config.h"
#include "chrome/browser/abp/abp_history_database.h"

namespace abp {

// Response callback type (same as AbpController)
using HistoryResponseCallback =
    base::OnceCallback<void(int status, std::string body)>;

// Controller for ABP history system.
// Manages session lifecycle, records actions/events, and handles REST requests.
// Must be created and used on the UI thread.
class AbpHistoryController {
 public:
  explicit AbpHistoryController(const AbpConfig& config);
  ~AbpHistoryController();

  AbpHistoryController(const AbpHistoryController&) = delete;
  AbpHistoryController& operator=(const AbpHistoryController&) = delete;

  // Initialize the history system (creates session, etc.)
  void Initialize();

  // Shutdown (ends session, flushes database)
  void Shutdown();

  // Check if history is enabled
  bool IsEnabled() const { return enabled_; }

  // Get current session ID
  const std::string& GetSessionId() const { return session_id_; }

  // Record an action after it completes
  // Called by AbpController after each action handler
  void RecordAction(const std::string& tab_id,
                    const std::string& action_type,
                    const base::Value::Dict& params,
                    const base::Value* result,
                    bool success,
                    const std::string& error_code,
                    const std::string& error_message,
                    int64_t start_time,
                    int64_t duration_ms,
                    const std::string& screenshot_before_path = "",
                    const std::string& screenshot_after_path = "");

  // Record a browser event
  // Called by AbpEventObserver
  void RecordEvent(const std::string& tab_id,
                   const std::string& event_type,
                   const base::Value::Dict& data);

  // Handle REST requests to /api/v1/history/*
  void HandleRequest(const std::string& method,
                     const std::string& path,
                     const std::string& body,
                     HistoryResponseCallback callback);

  // Get screenshot file for use by action recording
  // Returns path like /screenshots/{timestamp}_{tab_id}_{before|after}.webp
  base::FilePath GetScreenshotPath(const std::string& tab_id,
                                   int64_t timestamp,
                                   bool is_before);

  // Screenshot configuration access
  bool ScreenshotsEnabled() const {
    return enabled_ && config_.history.screenshots.enabled;
  }
  const base::FilePath& ScreenshotsDirectory() const {
    return config_.history.screenshots.directory;
  }

 private:
  // REST endpoint handlers
  void HandleGetSessions(const std::string& query,
                         HistoryResponseCallback callback);
  void HandleGetCurrentSession(HistoryResponseCallback callback);
  void HandleGetSession(const std::string& session_id,
                        HistoryResponseCallback callback);
  void HandleExportSession(const std::string& session_id,
                           const std::string& query,
                           HistoryResponseCallback callback);

  void HandleGetActions(const std::string& query,
                        HistoryResponseCallback callback);
  void HandleGetAction(int64_t action_id, HistoryResponseCallback callback);
  void HandleGetActionScreenshot(int64_t action_id,
                                 const std::string& type,
                                 HistoryResponseCallback callback);
  void HandleDeleteActions(const std::string& query,
                           HistoryResponseCallback callback);

  void HandleGetEvents(const std::string& query,
                       HistoryResponseCallback callback);
  void HandleGetEvent(int64_t event_id, HistoryResponseCallback callback);
  void HandleDeleteEvents(const std::string& query,
                          HistoryResponseCallback callback);

  void HandleDeleteAll(const std::string& query,
                       HistoryResponseCallback callback);

  // Callback handlers
  void OnSessionsResult(HistoryResponseCallback callback, SessionsResult result);
  void OnSessionResult(HistoryResponseCallback callback,
                       std::optional<SessionRecord> session);
  void OnActionsResult(HistoryResponseCallback callback, ActionsResult result);
  void OnActionResult(HistoryResponseCallback callback,
                      std::optional<ActionRecord> action);
  void OnEventsResult(HistoryResponseCallback callback, EventsResult result);
  void OnEventResult(HistoryResponseCallback callback,
                     std::optional<EventRecord> event);
  void OnDeleteResult(HistoryResponseCallback callback, DeleteResult result);
  void OnBulkDeleteResult(HistoryResponseCallback callback,
                          BulkDeleteResult result);

  // Screenshot file serving
  void OnActionForScreenshot(HistoryResponseCallback callback,
                             const std::string& type,
                             std::optional<ActionRecord> action);

  // Export helpers
  void OnExportSessionResult(HistoryResponseCallback callback,
                             const std::string& query,
                             std::optional<SessionRecord> session);
  void OnExportActionsResult(HistoryResponseCallback callback,
                             SessionRecord session,
                             const std::string& events_cursor,
                             int chunk_size,
                             bool include_screenshots,
                             ActionsResult actions);
  void OnExportEventsResult(HistoryResponseCallback callback,
                            SessionRecord session,
                            ActionsResult actions,
                            bool include_screenshots,
                            EventsResult events);

  // Helper methods
  void SendJson(int status, base::Value value, HistoryResponseCallback callback);
  void SendError(int status,
                 const std::string& error_code,
                 const std::string& message,
                 HistoryResponseCallback callback);
  void SendBinaryFile(const base::FilePath& path,
                      const std::string& content_type,
                      HistoryResponseCallback callback);

  // Parse query string into key-value pairs
  static std::map<std::string, std::string> ParseQuery(const std::string& query);

  // Generate UUID for session
  static std::string GenerateSessionId();

  AbpConfig config_;
  bool enabled_;
  std::string session_id_;
  int64_t session_start_time_ = 0;

  std::unique_ptr<AbpHistoryDatabase> database_;

  base::WeakPtrFactory<AbpHistoryController> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_HISTORY_CONTROLLER_H_
