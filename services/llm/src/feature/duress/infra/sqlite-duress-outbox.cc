#include "sqlite-duress-outbox.hxx"

#include <sqlite3.h>
#include <trantor/utils/Logger.h>

#include <string>
#include <utility>

SqliteDuressOutbox::SqliteDuressOutbox(SqliteDuressOutboxInput input)
    : mutex_(input.mutex), db_(std::move(input.db)), writer_(nullptr, sqlite3_close_v2)
{
}

sqlite3* SqliteDuressOutbox::writer()
{
  if (writer_)
    return writer_.get();
  std::string path;
  {
    const std::scoped_lock lock(*mutex_);
    sqlite3* const shared = db_();
    const char* file = shared != nullptr ? sqlite3_db_filename(shared, "main") : nullptr;
    if (file == nullptr || *file == '\0')
      return nullptr;
    path = file;
  }
  sqlite3* raw = nullptr;
  if (sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    sqlite3_close_v2(raw);
    LOG_WARN << "Safety relay: the alert writer could not open its own connection";
    return nullptr;
  }
  writer_.reset(raw);
  sqlite3_busy_timeout(raw, kWriterBusyTimeoutMs);
  if (!repository_.syncFully(raw))
    LOG_WARN << "Safety relay: the alert writer could not ask for a full sync";
  return raw;
}

std::optional<int64_t> SqliteDuressOutbox::add(const DuressRaiseCreateInput& input)
{
  if (mutex_ == nullptr || !db_)
    return std::nullopt;
  const std::scoped_lock lock(writeMutex_);
  sqlite3* const handle = writer();
  const std::optional<int64_t> id = handle != nullptr ? repository_.add(handle, input) : std::nullopt;
  if (!id)
    LOG_WARN << "Safety relay: an alert could not be written down; it waits in memory only";
  return id;
}

std::optional<std::vector<DuressRaiseRow>> SqliteDuressOutbox::pending()
{
  if (mutex_ == nullptr || !db_)
    return std::vector<DuressRaiseRow>{};
  const std::scoped_lock lock(*mutex_);
  sqlite3* const handle = db_();
  if (handle == nullptr)
    return std::nullopt;
  return repository_.pending(handle);
}

void SqliteDuressOutbox::remove(int64_t id)
{
  if (mutex_ == nullptr || !db_ || id <= 0)
    return;
  const std::scoped_lock lock(*mutex_);
  sqlite3* const handle = db_();
  if (handle == nullptr || !repository_.remove(handle, id))
    LOG_WARN << "Safety relay: a settled alert could not be crossed out; guard may see it again after a restart";
}

int SqliteDuressOutbox::synchronous()
{
  if (mutex_ == nullptr || !db_)
    return -1;
  const std::scoped_lock lock(writeMutex_);
  sqlite3* const handle = writer();
  return handle != nullptr ? repository_.synchronous(handle) : -1;
}
