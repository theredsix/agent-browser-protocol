#ifndef CHROME_BROWSER_ABP_ABP_HISTORY_DATABASE_H_
#define CHROME_BROWSER_ABP_ABP_HISTORY_DATABASE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"

namespace sql {
class Database;
}

namespace abp {

// Session record
struct SessionRecord {
  SessionRecord();
  ~SessionRecord();
  SessionRecord(const SessionRecord&);
  SessionRecord& operator=(const SessionRecord&);
  SessionRecord(SessionRecord&&);
  SessionRecord& operator=(SessionRecord&&);

  std::string id;           // UUID
  int64_t start_time = 0;   // Unix timestamp ms
  int64_t end_time = 0;     // 0 if active
  std::string browser_version;
  std::string user_agent;
};

// Action record
struct ActionRecord {
  ActionRecord();
  ~ActionRecord();
  ActionRecord(const ActionRecord&);
  ActionRecord& operator=(const ActionRecord&);
  ActionRecord(ActionRecord&&);
  ActionRecord& operator=(ActionRecord&&);

  int64_t id = 0;
  std::string session_id;
  std::string tab_id;
  std::string action_type;
  int64_t timestamp = 0;
  int64_t duration_ms = 0;
  std::string params_json;
  std::string result_json;
  bool success = false;
  std::string error_code;
  std::string error_message;
  std::string screenshot_before_path;
  std::string screenshot_after_path;
};

// Event record
struct EventRecord {
  EventRecord();
  ~EventRecord();
  EventRecord(const EventRecord&);
  EventRecord& operator=(const EventRecord&);
  EventRecord(EventRecord&&);
  EventRecord& operator=(EventRecord&&);

  int64_t id = 0;
  std::string session_id;
  std::string tab_id;
  std::string event_type;
  int64_t timestamp = 0;
  std::string data_json;
};

// Query filters for actions
struct ActionQueryFilter {
  ActionQueryFilter();
  ~ActionQueryFilter();
  ActionQueryFilter(const ActionQueryFilter&);
  ActionQueryFilter& operator=(const ActionQueryFilter&);

  std::string session_id;
  std::string tab_id;
  std::string action_type;
  std::optional<bool> success;
  int64_t start_time = 0;
  int64_t end_time = 0;
  int limit = 50;
  int64_t cursor = 0;
  bool forward = false;  // false = toward older (default), true = toward newer
};

// Query filters for events
struct EventQueryFilter {
  EventQueryFilter();
  ~EventQueryFilter();
  EventQueryFilter(const EventQueryFilter&);
  EventQueryFilter& operator=(const EventQueryFilter&);

  std::string session_id;
  std::string tab_id;
  std::string event_type;
  int64_t start_time = 0;
  int64_t end_time = 0;
  int limit = 100;
  int64_t cursor = 0;
  bool forward = false;
};

// Paginated result for sessions
struct SessionsResult {
  SessionsResult();
  ~SessionsResult();
  SessionsResult(SessionsResult&&);
  SessionsResult& operator=(SessionsResult&&);

  std::vector<SessionRecord> sessions;
  std::string prev_cursor;
  std::string next_cursor;
  bool has_more = false;
};

// Paginated result for actions
struct ActionsResult {
  ActionsResult();
  ~ActionsResult();
  ActionsResult(ActionsResult&&);
  ActionsResult& operator=(ActionsResult&&);

  std::vector<ActionRecord> actions;
  std::string prev_cursor;
  std::string next_cursor;
  bool has_more = false;
};

// Paginated result for events
struct EventsResult {
  EventsResult();
  ~EventsResult();
  EventsResult(EventsResult&&);
  EventsResult& operator=(EventsResult&&);

  std::vector<EventRecord> events;
  std::string prev_cursor;
  std::string next_cursor;
  bool has_more = false;
};

// Delete result
struct DeleteResult {
  int64_t deleted_count = 0;
};

// Bulk delete result
struct BulkDeleteResult {
  int64_t deleted_sessions = 0;
  int64_t deleted_actions = 0;
  int64_t deleted_events = 0;
};

// SQLite database wrapper for ABP history.
// All public methods are thread-safe and use PostTask internally.
// Must be created and destroyed on the UI thread.
class AbpHistoryDatabase {
 public:
  // Callback types
  using SessionCallback = base::OnceCallback<void(std::optional<SessionRecord>)>;
  using SessionsCallback = base::OnceCallback<void(SessionsResult)>;
  using ActionCallback = base::OnceCallback<void(std::optional<ActionRecord>)>;
  using ActionsCallback = base::OnceCallback<void(ActionsResult)>;
  using EventCallback = base::OnceCallback<void(std::optional<EventRecord>)>;
  using EventsCallback = base::OnceCallback<void(EventsResult)>;
  using DeleteCallback = base::OnceCallback<void(DeleteResult)>;
  using BulkDeleteCallback = base::OnceCallback<void(BulkDeleteResult)>;
  using VoidCallback = base::OnceCallback<void()>;
  using InsertActionCallback = base::OnceCallback<void(int64_t id)>;
  using InsertEventCallback = base::OnceCallback<void(int64_t id)>;

  explicit AbpHistoryDatabase(const base::FilePath& database_path);
  ~AbpHistoryDatabase();

  AbpHistoryDatabase(const AbpHistoryDatabase&) = delete;
  AbpHistoryDatabase& operator=(const AbpHistoryDatabase&) = delete;

  // Initialize the database (creates tables if needed)
  void Initialize(VoidCallback callback);

  // Flush pending writes on shutdown
  void FlushOnShutdown();

  // Session operations
  void InsertSession(const SessionRecord& session, VoidCallback callback);
  void UpdateSessionEndTime(const std::string& session_id,
                            int64_t end_time,
                            VoidCallback callback);
  void GetSession(const std::string& session_id, SessionCallback callback);
  void GetSessions(int limit,
                   const std::string& cursor,
                   bool forward,
                   bool active_only,
                   SessionsCallback callback);

  // Action operations
  void InsertAction(const ActionRecord& action, InsertActionCallback callback);
  void GetAction(int64_t action_id, ActionCallback callback);
  void GetActions(const ActionQueryFilter& filter, ActionsCallback callback);
  void DeleteActions(const std::string& session_id,
                     const std::string& tab_id,
                     int64_t before_time,
                     DeleteCallback callback);

  // Event operations
  void InsertEvent(const EventRecord& event, InsertEventCallback callback);
  void GetEvent(int64_t event_id, EventCallback callback);
  void GetEvents(const EventQueryFilter& filter, EventsCallback callback);
  void DeleteEvents(const std::string& session_id,
                    const std::string& tab_id,
                    const std::string& event_type,
                    int64_t before_time,
                    DeleteCallback callback);

  // Bulk operations
  void DeleteAll(const std::string& session_id,
                 bool confirm,
                 BulkDeleteCallback callback);

 private:
  // Database operations (run on db_task_runner_)
  void InitializeOnDB();
  bool CreateTables();

  void InsertSessionOnDB(SessionRecord session);
  void UpdateSessionEndTimeOnDB(std::string session_id, int64_t end_time);
  std::optional<SessionRecord> GetSessionOnDB(const std::string& session_id);
  SessionsResult GetSessionsOnDB(int limit,
                                 int64_t cursor,
                                 bool forward,
                                 bool active_only);

  int64_t InsertActionOnDB(ActionRecord action);
  std::optional<ActionRecord> GetActionOnDB(int64_t action_id);
  ActionsResult GetActionsOnDB(const ActionQueryFilter& filter);
  int64_t DeleteActionsOnDB(const std::string& session_id,
                            const std::string& tab_id,
                            int64_t before_time);

  int64_t InsertEventOnDB(EventRecord event);
  std::optional<EventRecord> GetEventOnDB(int64_t event_id);
  EventsResult GetEventsOnDB(const EventQueryFilter& filter);
  int64_t DeleteEventsOnDB(const std::string& session_id,
                           const std::string& tab_id,
                           const std::string& event_type,
                           int64_t before_time);

  BulkDeleteResult DeleteAllOnDB(const std::string& session_id, bool confirm);

  const base::FilePath database_path_;
  std::unique_ptr<sql::Database> db_;
  scoped_refptr<base::SequencedTaskRunner> db_task_runner_;
  scoped_refptr<base::SequencedTaskRunner> ui_task_runner_;

  SEQUENCE_CHECKER(ui_sequence_checker_);
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_HISTORY_DATABASE_H_
