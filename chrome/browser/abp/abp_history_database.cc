#include "chrome/browser/abp/abp_history_database.h"

#include <tuple>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_thread.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "sql/transaction.h"

namespace abp {

// Record struct implementations
SessionRecord::SessionRecord() = default;
SessionRecord::~SessionRecord() = default;
SessionRecord::SessionRecord(const SessionRecord&) = default;
SessionRecord& SessionRecord::operator=(const SessionRecord&) = default;
SessionRecord::SessionRecord(SessionRecord&&) = default;
SessionRecord& SessionRecord::operator=(SessionRecord&&) = default;

ActionRecord::ActionRecord() = default;
ActionRecord::~ActionRecord() = default;
ActionRecord::ActionRecord(const ActionRecord&) = default;
ActionRecord& ActionRecord::operator=(const ActionRecord&) = default;
ActionRecord::ActionRecord(ActionRecord&&) = default;
ActionRecord& ActionRecord::operator=(ActionRecord&&) = default;

EventRecord::EventRecord() = default;
EventRecord::~EventRecord() = default;
EventRecord::EventRecord(const EventRecord&) = default;
EventRecord& EventRecord::operator=(const EventRecord&) = default;
EventRecord::EventRecord(EventRecord&&) = default;
EventRecord& EventRecord::operator=(EventRecord&&) = default;

ActionQueryFilter::ActionQueryFilter() = default;
ActionQueryFilter::~ActionQueryFilter() = default;
ActionQueryFilter::ActionQueryFilter(const ActionQueryFilter&) = default;
ActionQueryFilter& ActionQueryFilter::operator=(const ActionQueryFilter&) = default;

EventQueryFilter::EventQueryFilter() = default;
EventQueryFilter::~EventQueryFilter() = default;
EventQueryFilter::EventQueryFilter(const EventQueryFilter&) = default;
EventQueryFilter& EventQueryFilter::operator=(const EventQueryFilter&) = default;

SessionsResult::SessionsResult() = default;
SessionsResult::~SessionsResult() = default;
SessionsResult::SessionsResult(SessionsResult&&) = default;
SessionsResult& SessionsResult::operator=(SessionsResult&&) = default;

ActionsResult::ActionsResult() = default;
ActionsResult::~ActionsResult() = default;
ActionsResult::ActionsResult(ActionsResult&&) = default;
ActionsResult& ActionsResult::operator=(ActionsResult&&) = default;

EventsResult::EventsResult() = default;
EventsResult::~EventsResult() = default;
EventsResult::EventsResult(EventsResult&&) = default;
EventsResult& EventsResult::operator=(EventsResult&&) = default;

namespace {

constexpr char kCreateSessionsTable[] = R"(
  CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    start_time INTEGER NOT NULL,
    end_time INTEGER,
    browser_version TEXT,
    user_agent TEXT
  )
)";

constexpr char kCreateActionsTable[] = R"(
  CREATE TABLE IF NOT EXISTS actions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL REFERENCES sessions(id),
    tab_id TEXT,
    action_type TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    duration_ms INTEGER,
    params TEXT,
    result TEXT,
    success INTEGER NOT NULL,
    error_code TEXT,
    error_message TEXT,
    screenshot_before_path TEXT,
    screenshot_after_path TEXT
  )
)";

constexpr char kCreateEventsTable[] = R"(
  CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL REFERENCES sessions(id),
    tab_id TEXT,
    event_type TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    data TEXT
  )
)";

}  // namespace

AbpHistoryDatabase::AbpHistoryDatabase(const base::FilePath& database_path)
    : database_path_(database_path) {
  DETACH_FROM_SEQUENCE(ui_sequence_checker_);

  // Create a sequenced task runner for database operations
  db_task_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
      {base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN, base::MayBlock()});

  ui_task_runner_ = content::GetUIThreadTaskRunner({});
}

AbpHistoryDatabase::~AbpHistoryDatabase() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
  // Database will be closed when destroyed on the DB thread
  if (db_) {
    db_task_runner_->DeleteSoon(FROM_HERE, std::move(db_));
  }
}

void AbpHistoryDatabase::Initialize(VoidCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::InitializeOnDB,
                     base::Unretained(this)),
      std::move(callback));
}

void AbpHistoryDatabase::FlushOnShutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  // Use a RunLoop to block until all pending DB operations complete
  base::RunLoop run_loop;
  db_task_runner_->PostTaskAndReply(FROM_HERE, base::DoNothing(),
                                    run_loop.QuitClosure());
  run_loop.Run();
}

void AbpHistoryDatabase::InitializeOnDB() {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());

  // Ensure parent directory exists
  base::FilePath dir = database_path_.DirName();
  if (!base::DirectoryExists(dir)) {
    if (!base::CreateDirectory(dir)) {
      LOG(ERROR) << "ABP: Failed to create database directory: " << dir;
      return;
    }
  }

  db_ = std::make_unique<sql::Database>(
      sql::DatabaseOptions().set_wal_mode(true),
      sql::Database::Tag("ABP"));

  if (!db_->Open(database_path_)) {
    LOG(ERROR) << "ABP: Failed to open database: " << database_path_;
    db_.reset();
    return;
  }

  // Create tables
  if (!CreateTables()) {
    LOG(ERROR) << "ABP: Failed to create tables";
    db_.reset();
    return;
  }

  LOG(INFO) << "ABP: History database initialized at " << database_path_;
}

bool AbpHistoryDatabase::CreateTables() {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  DCHECK(db_);

  sql::Transaction transaction(db_.get());
  if (!transaction.Begin()) {
    return false;
  }

  if (!db_->Execute(kCreateSessionsTable)) {
    return false;
  }

  if (!db_->Execute(kCreateActionsTable)) {
    return false;
  }

  if (!db_->Execute(kCreateEventsTable)) {
    return false;
  }

  // Create indexes (ignore failures - indexes are optional optimizations)
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_actions_session ON actions(session_id)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_actions_tab ON actions(tab_id)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_actions_type ON actions(action_type)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_actions_timestamp ON actions(timestamp)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_actions_session_timestamp ON "
      "actions(session_id, timestamp)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_actions_session_id ON "
      "actions(session_id, id)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_events_session ON events(session_id)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_events_tab ON events(tab_id)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_events_type ON events(event_type)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_events_timestamp ON events(timestamp)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_events_session_timestamp ON "
      "events(session_id, timestamp)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_events_session_id ON "
      "events(session_id, id)");

  return transaction.Commit();
}

// Session operations

void AbpHistoryDatabase::InsertSession(const SessionRecord& session,
                                       VoidCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::InsertSessionOnDB,
                     base::Unretained(this), session),
      std::move(callback));
}

void AbpHistoryDatabase::InsertSessionOnDB(SessionRecord session) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return;
  }

  sql::Statement stmt(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "INSERT INTO sessions (id, start_time, end_time, browser_version, "
      "user_agent) VALUES (?, ?, ?, ?, ?)"));

  stmt.BindString(0, session.id);
  stmt.BindInt64(1, session.start_time);
  if (session.end_time > 0) {
    stmt.BindInt64(2, session.end_time);
  } else {
    stmt.BindNull(2);
  }
  stmt.BindString(3, session.browser_version);
  stmt.BindString(4, session.user_agent);

  if (!stmt.Run()) {
    LOG(ERROR) << "ABP: Failed to insert session: " << session.id;
  }
}

void AbpHistoryDatabase::UpdateSessionEndTime(const std::string& session_id,
                                              int64_t end_time,
                                              VoidCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::UpdateSessionEndTimeOnDB,
                     base::Unretained(this), session_id, end_time),
      std::move(callback));
}

void AbpHistoryDatabase::UpdateSessionEndTimeOnDB(std::string session_id,
                                                  int64_t end_time) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return;
  }

  sql::Statement stmt(db_->GetCachedStatement(
      SQL_FROM_HERE, "UPDATE sessions SET end_time = ? WHERE id = ?"));

  stmt.BindInt64(0, end_time);
  stmt.BindString(1, session_id);

  if (!stmt.Run()) {
    LOG(ERROR) << "ABP: Failed to update session end time: " << session_id;
  }
}

void AbpHistoryDatabase::GetSession(const std::string& session_id,
                                    SessionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::GetSessionOnDB,
                     base::Unretained(this), session_id),
      std::move(callback));
}

std::optional<SessionRecord> AbpHistoryDatabase::GetSessionOnDB(
    const std::string& session_id) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return std::nullopt;
  }

  sql::Statement stmt(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT id, start_time, end_time, browser_version, user_agent "
      "FROM sessions WHERE id = ?"));

  stmt.BindString(0, session_id);

  if (!stmt.Step()) {
    return std::nullopt;
  }

  SessionRecord record;
  record.id = stmt.ColumnString(0);
  record.start_time = stmt.ColumnInt64(1);
  record.end_time = stmt.GetColumnType(2) == sql::ColumnType::kNull
                        ? 0
                        : stmt.ColumnInt64(2);
  record.browser_version = stmt.ColumnString(3);
  record.user_agent = stmt.ColumnString(4);

  return record;
}

void AbpHistoryDatabase::GetSessions(int limit,
                                     const std::string& cursor,
                                     bool forward,
                                     bool active_only,
                                     SessionsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  int64_t cursor_value = 0;
  if (!cursor.empty()) {
    base::StringToInt64(cursor, &cursor_value);
  }

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::GetSessionsOnDB,
                     base::Unretained(this), limit, cursor_value, forward,
                     active_only),
      std::move(callback));
}

SessionsResult AbpHistoryDatabase::GetSessionsOnDB(int limit,
                                                   int64_t cursor,
                                                   bool forward,
                                                   bool active_only) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  SessionsResult result;

  if (!db_) {
    return result;
  }

  // Build query based on cursor and direction
  std::string query =
      "SELECT id, start_time, end_time, browser_version, user_agent "
      "FROM sessions WHERE 1=1";

  if (active_only) {
    query += " AND end_time IS NULL";
  }

  if (cursor > 0) {
    if (forward) {
      query += " AND start_time > ?";
    } else {
      query += " AND start_time < ?";
    }
  }

  if (forward) {
    query += " ORDER BY start_time ASC";
  } else {
    query += " ORDER BY start_time DESC";
  }

  query += " LIMIT ?";

  sql::Statement stmt(db_->GetUniqueStatement(query));

  int param_idx = 0;
  if (cursor > 0) {
    stmt.BindInt64(param_idx++, cursor);
  }
  stmt.BindInt(param_idx, limit + 1);  // Fetch one extra to check has_more

  while (stmt.Step()) {
    if (static_cast<int>(result.sessions.size()) >= limit) {
      result.has_more = true;
      break;
    }

    SessionRecord record;
    record.id = stmt.ColumnString(0);
    record.start_time = stmt.ColumnInt64(1);
    record.end_time = stmt.GetColumnType(2) == sql::ColumnType::kNull
                          ? 0
                          : stmt.ColumnInt64(2);
    record.browser_version = stmt.ColumnString(3);
    record.user_agent = stmt.ColumnString(4);
    result.sessions.push_back(std::move(record));
  }

  // Set cursors
  if (!result.sessions.empty()) {
    if (forward) {
      result.prev_cursor = base::NumberToString(result.sessions.front().start_time);
      if (result.has_more) {
        result.next_cursor = base::NumberToString(result.sessions.back().start_time);
      }
    } else {
      if (result.has_more) {
        result.prev_cursor = base::NumberToString(result.sessions.back().start_time);
      }
      result.next_cursor = base::NumberToString(result.sessions.front().start_time);
    }
  }

  return result;
}

// Action operations

void AbpHistoryDatabase::InsertAction(const ActionRecord& action,
                                      InsertActionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::InsertActionOnDB,
                     base::Unretained(this), action),
      std::move(callback));
}

int64_t AbpHistoryDatabase::InsertActionOnDB(ActionRecord action) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return 0;
  }

  sql::Statement stmt(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "INSERT INTO actions (session_id, tab_id, action_type, timestamp, "
      "duration_ms, params, result, success, error_code, error_message, "
      "screenshot_before_path, screenshot_after_path) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));

  stmt.BindString(0, action.session_id);
  stmt.BindString(1, action.tab_id);
  stmt.BindString(2, action.action_type);
  stmt.BindInt64(3, action.timestamp);
  stmt.BindInt64(4, action.duration_ms);
  stmt.BindString(5, action.params_json);
  stmt.BindString(6, action.result_json);
  stmt.BindInt(7, action.success ? 1 : 0);
  stmt.BindString(8, action.error_code);
  stmt.BindString(9, action.error_message);
  stmt.BindString(10, action.screenshot_before_path);
  stmt.BindString(11, action.screenshot_after_path);

  if (!stmt.Run()) {
    LOG(ERROR) << "ABP: Failed to insert action";
    return 0;
  }

  return db_->GetLastInsertRowId();
}

void AbpHistoryDatabase::GetAction(int64_t action_id, ActionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::GetActionOnDB, base::Unretained(this),
                     action_id),
      std::move(callback));
}

std::optional<ActionRecord> AbpHistoryDatabase::GetActionOnDB(
    int64_t action_id) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return std::nullopt;
  }

  sql::Statement stmt(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT id, session_id, tab_id, action_type, timestamp, duration_ms, "
      "params, result, success, error_code, error_message, "
      "screenshot_before_path, screenshot_after_path "
      "FROM actions WHERE id = ?"));

  stmt.BindInt64(0, action_id);

  if (!stmt.Step()) {
    return std::nullopt;
  }

  ActionRecord record;
  record.id = stmt.ColumnInt64(0);
  record.session_id = stmt.ColumnString(1);
  record.tab_id = stmt.ColumnString(2);
  record.action_type = stmt.ColumnString(3);
  record.timestamp = stmt.ColumnInt64(4);
  record.duration_ms = stmt.ColumnInt64(5);
  record.params_json = stmt.ColumnString(6);
  record.result_json = stmt.ColumnString(7);
  record.success = stmt.ColumnInt(8) != 0;
  record.error_code = stmt.ColumnString(9);
  record.error_message = stmt.ColumnString(10);
  record.screenshot_before_path = stmt.ColumnString(11);
  record.screenshot_after_path = stmt.ColumnString(12);

  return record;
}

void AbpHistoryDatabase::GetActions(const ActionQueryFilter& filter,
                                    ActionsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::GetActionsOnDB,
                     base::Unretained(this), filter),
      std::move(callback));
}

ActionsResult AbpHistoryDatabase::GetActionsOnDB(
    const ActionQueryFilter& filter) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  ActionsResult result;

  if (!db_) {
    return result;
  }

  // Build query with filters
  std::string query =
      "SELECT id, session_id, tab_id, action_type, timestamp, duration_ms, "
      "params, result, success, error_code, error_message, "
      "screenshot_before_path, screenshot_after_path "
      "FROM actions WHERE 1=1";

  std::vector<std::string> string_binds;
  std::vector<int64_t> int_binds;

  if (!filter.session_id.empty()) {
    query += " AND session_id = ?";
    string_binds.push_back(filter.session_id);
  }
  if (!filter.tab_id.empty()) {
    query += " AND tab_id = ?";
    string_binds.push_back(filter.tab_id);
  }
  if (!filter.action_type.empty()) {
    query += " AND action_type = ?";
    string_binds.push_back(filter.action_type);
  }
  if (filter.success.has_value()) {
    query += " AND success = ?";
    int_binds.push_back(*filter.success ? 1 : 0);
  }
  if (filter.start_time > 0) {
    query += " AND timestamp >= ?";
    int_binds.push_back(filter.start_time);
  }
  if (filter.end_time > 0) {
    query += " AND timestamp <= ?";
    int_binds.push_back(filter.end_time);
  }

  if (filter.cursor > 0) {
    if (filter.forward) {
      query += " AND id > ?";
    } else {
      query += " AND id < ?";
    }
    int_binds.push_back(filter.cursor);
  }

  if (filter.forward) {
    query += " ORDER BY id ASC";
  } else {
    query += " ORDER BY id DESC";
  }

  query += " LIMIT ?";
  int_binds.push_back(filter.limit + 1);

  sql::Statement stmt(db_->GetUniqueStatement(query));

  int param_idx = 0;
  for (const auto& s : string_binds) {
    stmt.BindString(param_idx++, s);
  }
  for (int64_t i : int_binds) {
    stmt.BindInt64(param_idx++, i);
  }

  while (stmt.Step()) {
    if (static_cast<int>(result.actions.size()) >= filter.limit) {
      result.has_more = true;
      break;
    }

    ActionRecord record;
    record.id = stmt.ColumnInt64(0);
    record.session_id = stmt.ColumnString(1);
    record.tab_id = stmt.ColumnString(2);
    record.action_type = stmt.ColumnString(3);
    record.timestamp = stmt.ColumnInt64(4);
    record.duration_ms = stmt.ColumnInt64(5);
    record.params_json = stmt.ColumnString(6);
    record.result_json = stmt.ColumnString(7);
    record.success = stmt.ColumnInt(8) != 0;
    record.error_code = stmt.ColumnString(9);
    record.error_message = stmt.ColumnString(10);
    record.screenshot_before_path = stmt.ColumnString(11);
    record.screenshot_after_path = stmt.ColumnString(12);
    result.actions.push_back(std::move(record));
  }

  // Set cursors
  if (!result.actions.empty()) {
    if (filter.forward) {
      result.prev_cursor = base::NumberToString(result.actions.front().id);
      if (result.has_more) {
        result.next_cursor = base::NumberToString(result.actions.back().id);
      }
    } else {
      if (result.has_more) {
        result.prev_cursor = base::NumberToString(result.actions.back().id);
      }
      result.next_cursor = base::NumberToString(result.actions.front().id);
    }
  }

  return result;
}

void AbpHistoryDatabase::DeleteActions(const std::string& session_id,
                                       const std::string& tab_id,
                                       int64_t before_time,
                                       DeleteCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::DeleteActionsOnDB,
                     base::Unretained(this), session_id, tab_id, before_time),
      base::BindOnce([](DeleteCallback cb,
                        int64_t count) { std::move(cb).Run({count}); },
                     std::move(callback)));
}

int64_t AbpHistoryDatabase::DeleteActionsOnDB(const std::string& session_id,
                                              const std::string& tab_id,
                                              int64_t before_time) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return 0;
  }

  std::string query = "DELETE FROM actions WHERE 1=1";
  std::vector<std::string> string_binds;
  std::vector<int64_t> int_binds;

  if (!session_id.empty()) {
    query += " AND session_id = ?";
    string_binds.push_back(session_id);
  }
  if (!tab_id.empty()) {
    query += " AND tab_id = ?";
    string_binds.push_back(tab_id);
  }
  if (before_time > 0) {
    query += " AND timestamp < ?";
    int_binds.push_back(before_time);
  }

  sql::Statement stmt(db_->GetUniqueStatement(query));

  int param_idx = 0;
  for (const auto& s : string_binds) {
    stmt.BindString(param_idx++, s);
  }
  for (int64_t i : int_binds) {
    stmt.BindInt64(param_idx++, i);
  }

  if (!stmt.Run()) {
    return 0;
  }

  return db_->GetLastChangeCount();
}

// Event operations

void AbpHistoryDatabase::InsertEvent(const EventRecord& event,
                                     InsertEventCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::InsertEventOnDB,
                     base::Unretained(this), event),
      std::move(callback));
}

int64_t AbpHistoryDatabase::InsertEventOnDB(EventRecord event) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return 0;
  }

  sql::Statement stmt(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "INSERT INTO events (session_id, tab_id, event_type, timestamp, data) "
      "VALUES (?, ?, ?, ?, ?)"));

  stmt.BindString(0, event.session_id);
  stmt.BindString(1, event.tab_id);
  stmt.BindString(2, event.event_type);
  stmt.BindInt64(3, event.timestamp);
  stmt.BindString(4, event.data_json);

  if (!stmt.Run()) {
    LOG(ERROR) << "ABP: Failed to insert event";
    return 0;
  }

  return db_->GetLastInsertRowId();
}

void AbpHistoryDatabase::GetEvent(int64_t event_id, EventCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::GetEventOnDB, base::Unretained(this),
                     event_id),
      std::move(callback));
}

std::optional<EventRecord> AbpHistoryDatabase::GetEventOnDB(int64_t event_id) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return std::nullopt;
  }

  sql::Statement stmt(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT id, session_id, tab_id, event_type, timestamp, data "
      "FROM events WHERE id = ?"));

  stmt.BindInt64(0, event_id);

  if (!stmt.Step()) {
    return std::nullopt;
  }

  EventRecord record;
  record.id = stmt.ColumnInt64(0);
  record.session_id = stmt.ColumnString(1);
  record.tab_id = stmt.ColumnString(2);
  record.event_type = stmt.ColumnString(3);
  record.timestamp = stmt.ColumnInt64(4);
  record.data_json = stmt.ColumnString(5);

  return record;
}

void AbpHistoryDatabase::GetEvents(const EventQueryFilter& filter,
                                   EventsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::GetEventsOnDB, base::Unretained(this),
                     filter),
      std::move(callback));
}

EventsResult AbpHistoryDatabase::GetEventsOnDB(const EventQueryFilter& filter) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  EventsResult result;

  if (!db_) {
    return result;
  }

  // Build query with filters
  std::string query =
      "SELECT id, session_id, tab_id, event_type, timestamp, data "
      "FROM events WHERE 1=1";

  std::vector<std::string> string_binds;
  std::vector<int64_t> int_binds;

  if (!filter.session_id.empty()) {
    query += " AND session_id = ?";
    string_binds.push_back(filter.session_id);
  }
  if (!filter.tab_id.empty()) {
    query += " AND tab_id = ?";
    string_binds.push_back(filter.tab_id);
  }
  if (!filter.event_type.empty()) {
    query += " AND event_type = ?";
    string_binds.push_back(filter.event_type);
  }
  if (filter.start_time > 0) {
    query += " AND timestamp >= ?";
    int_binds.push_back(filter.start_time);
  }
  if (filter.end_time > 0) {
    query += " AND timestamp <= ?";
    int_binds.push_back(filter.end_time);
  }

  if (filter.cursor > 0) {
    if (filter.forward) {
      query += " AND id > ?";
    } else {
      query += " AND id < ?";
    }
    int_binds.push_back(filter.cursor);
  }

  if (filter.forward) {
    query += " ORDER BY id ASC";
  } else {
    query += " ORDER BY id DESC";
  }

  query += " LIMIT ?";
  int_binds.push_back(filter.limit + 1);

  sql::Statement stmt(db_->GetUniqueStatement(query));

  int param_idx = 0;
  for (const auto& s : string_binds) {
    stmt.BindString(param_idx++, s);
  }
  for (int64_t i : int_binds) {
    stmt.BindInt64(param_idx++, i);
  }

  while (stmt.Step()) {
    if (static_cast<int>(result.events.size()) >= filter.limit) {
      result.has_more = true;
      break;
    }

    EventRecord record;
    record.id = stmt.ColumnInt64(0);
    record.session_id = stmt.ColumnString(1);
    record.tab_id = stmt.ColumnString(2);
    record.event_type = stmt.ColumnString(3);
    record.timestamp = stmt.ColumnInt64(4);
    record.data_json = stmt.ColumnString(5);
    result.events.push_back(std::move(record));
  }

  // Set cursors
  if (!result.events.empty()) {
    if (filter.forward) {
      result.prev_cursor = base::NumberToString(result.events.front().id);
      if (result.has_more) {
        result.next_cursor = base::NumberToString(result.events.back().id);
      }
    } else {
      if (result.has_more) {
        result.prev_cursor = base::NumberToString(result.events.back().id);
      }
      result.next_cursor = base::NumberToString(result.events.front().id);
    }
  }

  return result;
}

void AbpHistoryDatabase::DeleteEvents(const std::string& session_id,
                                      const std::string& tab_id,
                                      const std::string& event_type,
                                      int64_t before_time,
                                      DeleteCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::DeleteEventsOnDB,
                     base::Unretained(this), session_id, tab_id, event_type,
                     before_time),
      base::BindOnce([](DeleteCallback cb,
                        int64_t count) { std::move(cb).Run({count}); },
                     std::move(callback)));
}

int64_t AbpHistoryDatabase::DeleteEventsOnDB(const std::string& session_id,
                                             const std::string& tab_id,
                                             const std::string& event_type,
                                             int64_t before_time) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return 0;
  }

  std::string query = "DELETE FROM events WHERE 1=1";
  std::vector<std::string> string_binds;
  std::vector<int64_t> int_binds;

  if (!session_id.empty()) {
    query += " AND session_id = ?";
    string_binds.push_back(session_id);
  }
  if (!tab_id.empty()) {
    query += " AND tab_id = ?";
    string_binds.push_back(tab_id);
  }
  if (!event_type.empty()) {
    query += " AND event_type = ?";
    string_binds.push_back(event_type);
  }
  if (before_time > 0) {
    query += " AND timestamp < ?";
    int_binds.push_back(before_time);
  }

  sql::Statement stmt(db_->GetUniqueStatement(query));

  int param_idx = 0;
  for (const auto& s : string_binds) {
    stmt.BindString(param_idx++, s);
  }
  for (int64_t i : int_binds) {
    stmt.BindInt64(param_idx++, i);
  }

  if (!stmt.Run()) {
    return 0;
  }

  return db_->GetLastChangeCount();
}

// Bulk operations

void AbpHistoryDatabase::DeleteAll(const std::string& session_id,
                                   bool confirm,
                                   BulkDeleteCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::DeleteAllOnDB, base::Unretained(this),
                     session_id, confirm),
      std::move(callback));
}

BulkDeleteResult AbpHistoryDatabase::DeleteAllOnDB(
    const std::string& session_id,
    bool confirm) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  BulkDeleteResult result;

  if (!db_ || !confirm) {
    return result;
  }

  sql::Transaction transaction(db_.get());
  if (!transaction.Begin()) {
    return result;
  }

  if (session_id.empty()) {
    // Delete all
    {
      sql::Statement stmt(
          db_->GetCachedStatement(SQL_FROM_HERE, "DELETE FROM events"));
      if (stmt.Run()) {
        result.deleted_events = db_->GetLastChangeCount();
      }
    }
    {
      sql::Statement stmt(
          db_->GetCachedStatement(SQL_FROM_HERE, "DELETE FROM actions"));
      if (stmt.Run()) {
        result.deleted_actions = db_->GetLastChangeCount();
      }
    }
    {
      sql::Statement stmt(
          db_->GetCachedStatement(SQL_FROM_HERE, "DELETE FROM sessions"));
      if (stmt.Run()) {
        result.deleted_sessions = db_->GetLastChangeCount();
      }
    }
  } else {
    // Delete for specific session
    {
      sql::Statement stmt(db_->GetCachedStatement(
          SQL_FROM_HERE, "DELETE FROM events WHERE session_id = ?"));
      stmt.BindString(0, session_id);
      if (stmt.Run()) {
        result.deleted_events = db_->GetLastChangeCount();
      }
    }
    {
      sql::Statement stmt(db_->GetCachedStatement(
          SQL_FROM_HERE, "DELETE FROM actions WHERE session_id = ?"));
      stmt.BindString(0, session_id);
      if (stmt.Run()) {
        result.deleted_actions = db_->GetLastChangeCount();
      }
    }
    {
      sql::Statement stmt(db_->GetCachedStatement(
          SQL_FROM_HERE, "DELETE FROM sessions WHERE id = ?"));
      stmt.BindString(0, session_id);
      if (stmt.Run()) {
        result.deleted_sessions = db_->GetLastChangeCount();
      }
    }
  }

  transaction.Commit();
  return result;
}

}  // namespace abp
