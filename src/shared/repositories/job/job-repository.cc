#include <drogon/drogon.h>
#include <shared/repositories/job/job-repository.hxx>
#include <shared/utils/schema-runner/schema-runner.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>

using namespace job_query;

bool JobRepository::open(const std::string& dbPath)
{
  std::scoped_lock lock(mutex_);
  if (db_)
    return true;
  sqlite3* raw = nullptr;
  if (sqlite3_open(dbPath.c_str(), &raw) != SQLITE_OK) {
    LOG_ERROR << "JobRepository: open failed: "
              << (raw ? sqlite3_errmsg(raw) : "no handle");
    if (raw)
      sqlite3_close(raw);
    return false;
  }
  db_.reset(raw);
  sqlite3_busy_timeout(db_.get(), 5000);
  {
    SqliteStmt stmt;
    const bool hasJobTable =
        stmt.prepare(db_.get(),
                     "SELECT 1 FROM sqlite_master WHERE type = 'table' AND "
                     "name = 'job'") &&
        stmt.step() == SQLITE_ROW;
    if (!hasJobTable)
      runSchemaFile(db_.get(), "database/schema.sql");
  }
  return true;
}

void JobRepository::close()
{
  std::scoped_lock lock(mutex_);
  db_.reset();
}

std::optional<int64_t> JobRepository::insert(const JobInsertInput& input)
{
  std::scoped_lock lock(mutex_);
  if (!db_)
    return std::nullopt;
  SqliteStmt stmt;
  if (!stmt.prepare(db_.get(), INSERT_JOB))
    return std::nullopt;
  stmt.bindText(1, input.queue);
  stmt.bindText(2, input.payload);
  stmt.bindText(3, jobStateToString(JobState::Waiting));
  stmt.bindInt(4, input.priority);
  stmt.bindInt(5, input.maxAttempts);
  if (input.dedupeKey.empty())
    stmt.bindNull(6);
  else
    stmt.bindText(6, input.dedupeKey);
  if (input.nextRunAt > 0)
    stmt.bindInt64(7, input.nextRunAt);
  else
    stmt.bindNull(7);
  stmt.bindInt64(8, input.createdAt);
  stmt.bindInt64(9, input.createdAt);
  if (stmt.step() != SQLITE_DONE)
    return std::nullopt;
  return sqlite3_last_insert_rowid(db_.get());
}

std::optional<int64_t>
JobRepository::findIdByDedupe(const std::string& queue,
                              const std::string& dedupeKey)
{
  std::scoped_lock lock(mutex_);
  if (!db_ || dedupeKey.empty())
    return std::nullopt;
  SqliteStmt stmt;
  if (!stmt.prepare(db_.get(), FIND_ID_BY_DEDUPE))
    return std::nullopt;
  stmt.bindText(1, queue);
  stmt.bindText(2, dedupeKey);
  if (stmt.step() != SQLITE_ROW)
    return std::nullopt;
  return stmt.columnInt64(0);
}

std::vector<JobRow> JobRepository::loadDue(const std::string& queue,
                                           int64_t now)
{
  std::vector<JobRow> rows;
  std::scoped_lock lock(mutex_);
  if (!db_)
    return rows;
  SqliteStmt stmt;
  if (!stmt.prepare(db_.get(), LOAD_DUE))
    return rows;
  stmt.bindText(1, queue);
  stmt.bindInt64(2, now);
  while (stmt.step() == SQLITE_ROW) {
    JobRow row;
    row.id = stmt.columnInt64(0);
    row.queue = stmt.columnText(1);
    row.payload = stmt.columnText(2);
    row.priority = stmt.columnInt(3);
    row.attempts = stmt.columnInt(4);
    row.maxAttempts = stmt.columnInt(5);
    row.createdAt = stmt.columnInt64(6);
    rows.push_back(std::move(row));
  }
  return rows;
}

bool JobRepository::markActive(int64_t id, int64_t now)
{
  std::scoped_lock lock(mutex_);
  if (!db_)
    return false;
  SqliteStmt stmt;
  if (!stmt.prepare(db_.get(), MARK_ACTIVE))
    return false;
  stmt.bindInt64(1, now);
  stmt.bindInt64(2, id);
  return stmt.step() == SQLITE_DONE;
}

bool JobRepository::markDelayed(int64_t id, int attempts,
                                const std::string& error, int64_t nextRunAt,
                                int64_t now)
{
  std::scoped_lock lock(mutex_);
  if (!db_)
    return false;
  SqliteStmt stmt;
  if (!stmt.prepare(db_.get(), MARK_DELAYED))
    return false;
  stmt.bindInt(1, attempts);
  if (error.empty())
    stmt.bindNull(2);
  else
    stmt.bindText(2, error);
  stmt.bindInt64(3, nextRunAt);
  stmt.bindInt64(4, now);
  stmt.bindInt64(5, id);
  return stmt.step() == SQLITE_DONE;
}

bool JobRepository::markFailed(int64_t id, int attempts,
                               const std::string& error, int64_t now)
{
  std::scoped_lock lock(mutex_);
  if (!db_)
    return false;
  SqliteStmt stmt;
  if (!stmt.prepare(db_.get(), MARK_FAILED))
    return false;
  stmt.bindInt(1, attempts);
  if (error.empty())
    stmt.bindNull(2);
  else
    stmt.bindText(2, error);
  stmt.bindInt64(3, now);
  stmt.bindInt64(4, id);
  return stmt.step() == SQLITE_DONE;
}

bool JobRepository::complete(int64_t id)
{
  std::scoped_lock lock(mutex_);
  if (!db_)
    return false;
  SqliteStmt stmt;
  if (!stmt.prepare(db_.get(), DELETE_JOB))
    return false;
  stmt.bindInt64(1, id);
  return stmt.step() == SQLITE_DONE;
}

int JobRepository::resetActive(int64_t now)
{
  std::scoped_lock lock(mutex_);
  if (!db_)
    return 0;
  SqliteStmt stmt;
  if (!stmt.prepare(db_.get(), RESET_ACTIVE))
    return 0;
  stmt.bindInt64(1, now);
  if (stmt.step() != SQLITE_DONE)
    return 0;
  return sqlite3_changes(db_.get());
}

int JobRepository::countByState(const std::string& queue,
                                const std::string& state)
{
  std::scoped_lock lock(mutex_);
  if (!db_)
    return 0;
  SqliteStmt stmt;
  if (!stmt.prepare(db_.get(), COUNT_BY_STATE))
    return 0;
  stmt.bindText(1, queue);
  stmt.bindText(2, state);
  if (stmt.step() != SQLITE_ROW)
    return 0;
  return stmt.columnInt(0);
}
