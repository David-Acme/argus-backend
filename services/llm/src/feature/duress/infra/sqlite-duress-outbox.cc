#include "sqlite-duress-outbox.hxx"

#include <trantor/utils/Logger.h>

#include <utility>

SqliteDuressOutbox::SqliteDuressOutbox(SqliteDuressOutboxInput input)
    : mutex_(input.mutex), db_(std::move(input.db))
{
}

std::optional<int64_t> SqliteDuressOutbox::add(const DuressRaiseCreateInput& input)
{
  if (mutex_ == nullptr || !db_)
    return std::nullopt;
  const std::scoped_lock lock(*mutex_);
  sqlite3* const handle = db_();
  const std::optional<int64_t> id = handle != nullptr ? repository_.add(handle, input) : std::nullopt;
  if (!id)
    LOG_WARN << "Safety relay: an alert could not be written down; it waits in memory only";
  return id;
}

std::vector<DuressRaiseRow> SqliteDuressOutbox::pending()
{
  if (mutex_ == nullptr || !db_)
    return {};
  const std::scoped_lock lock(*mutex_);
  sqlite3* const handle = db_();
  return handle != nullptr ? repository_.pending(handle) : std::vector<DuressRaiseRow>{};
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
