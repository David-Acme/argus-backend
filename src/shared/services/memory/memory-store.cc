#include "memory-store.hxx"

#include <drogon/drogon.h>
#include <shared/services/memory/simhash.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <sqlite3.h>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr int kDedupMaxHamming = 14;

void bindTextOrNull(sqlite3_stmt* stmt, int index,
                    const std::optional<int64_t>& value)
{
  if (value)
    sqlite3_bind_int64(stmt, index, *value);
  else
    sqlite3_bind_null(stmt, index);
}

bool stepDone(sqlite3* db, sqlite3_stmt* stmt)
{
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    LOG_WARN << "MemoryStore: sqlite step failed: " << sqlite3_errmsg(db);
    return false;
  }
  return true;
}

int64_t insertLocked(sqlite3* db, const MemorySaveInput& input)
{
  if (!db)
    return -1;

  const std::string scope = memoryScopeToString(input.scope);
  const std::string type = memoryTypeToString(input.type);
  const std::string source = memorySourceToString(input.source);

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "INSERT INTO memory_l1 (scope, ref_id, type, content, priority, "
          "source, source_turn_id, hit_count, lang, created_at, updated_at) "
          "VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, strftime('%s', 'now'), "
          "strftime('%s', 'now'))",
          -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "MemoryStore::save prepare failed: " << sqlite3_errmsg(db);
    return -1;
  }
  sqlite3_bind_text(stmt, 1, scope.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, input.refId);
  sqlite3_bind_text(stmt, 3, type.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, input.content.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 5, input.priority);
  sqlite3_bind_text(stmt, 6, source.data(), -1, SQLITE_TRANSIENT);
  bindTextOrNull(stmt, 7, input.sourceTurnId);
  sqlite3_bind_text(stmt, 8, input.lang.data(), -1, SQLITE_TRANSIENT);
  if (!stepDone(db, stmt))
    return -1;
  const int64_t id = static_cast<int64_t>(sqlite3_last_insert_rowid(db));

  if (sqlite3_prepare_v2(
          db,
          "INSERT INTO memory_fts_words (rowid, content, type, scope, "
          "ref_id) VALUES (?, ?, ?, ?, ?)",
          -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "MemoryStore: fts words prepare failed: " << sqlite3_errmsg(db);
    return -1;
  }
  sqlite3_bind_int64(stmt, 1, id);
  sqlite3_bind_text(stmt, 2, input.content.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, type.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, scope.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, input.refId);
  if (!stepDone(db, stmt))
    return -1;

  if (sqlite3_prepare_v2(
          db,
          "INSERT INTO memory_fts_grams (rowid, content, type, scope, "
          "ref_id) VALUES (?, ?, ?, ?, ?)",
          -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "MemoryStore: fts grams prepare failed: " << sqlite3_errmsg(db);
    return -1;
  }
  sqlite3_bind_int64(stmt, 1, id);
  sqlite3_bind_text(stmt, 2, input.content.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, type.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, scope.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, input.refId);
  if (!stepDone(db, stmt))
    return -1;

  return id;
}

void bumpMemory(sqlite3* db, int64_t id, int priority)
{
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "UPDATE memory_l1 SET priority = MAX(priority, ?), "
          "hit_count = hit_count + 1, updated_at = strftime('%s', 'now') "
          "WHERE id = ?",
          -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, priority);
    sqlite3_bind_int64(stmt, 2, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

} // namespace

int64_t MemoryStore::save(const MemorySaveInput& input)
{
  std::scoped_lock lock(VecDb::mutex());
  return insertLocked(VecDb::handle(), input);
}

int64_t MemoryStore::saveDedup(const MemorySaveInput& input)
{
  std::scoped_lock lock(VecDb::mutex());
  sqlite3* db = VecDb::handle();
  if (!db)
    return -1;

  const std::string scope = memoryScopeToString(input.scope);
  const uint64_t hash = SimHash::hash(input.content);

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "SELECT id, content FROM memory_l1 "
          "WHERE scope = ? AND ref_id = ? AND deleted_at IS NULL",
          -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "MemoryStore::saveDedup prepare failed: " << sqlite3_errmsg(db);
    return -1;
  }
  sqlite3_bind_text(stmt, 1, scope.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, input.refId);

  int64_t matchId = -1;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const int64_t id = sqlite3_column_int64(stmt, 0);
    const char* raw =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!raw)
      continue;
    if (SimHash::distance(hash, SimHash::hash(raw)) <= kDedupMaxHamming) {
      matchId = id;
      break;
    }
  }
  sqlite3_finalize(stmt);

  if (matchId > 0) {
    bumpMemory(db, matchId, input.priority);
    return matchId;
  }
  return insertLocked(db, input);
}

bool MemoryStore::remove(int64_t id)
{
  std::scoped_lock lock(VecDb::mutex());
  sqlite3* db = VecDb::handle();
  if (!db)
    return false;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "UPDATE memory_l1 SET deleted_at = strftime('%s', 'now') "
          "WHERE id = ?",
          -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "MemoryStore::remove prepare failed: " << sqlite3_errmsg(db);
    return false;
  }
  sqlite3_bind_int64(stmt, 1, id);
  if (!stepDone(db, stmt))
    return false;

  if (sqlite3_prepare_v2(db, "DELETE FROM memory_fts_words WHERE rowid = ?",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "MemoryStore: fts words delete prepare failed: "
             << sqlite3_errmsg(db);
    return false;
  }
  sqlite3_bind_int64(stmt, 1, id);
  if (!stepDone(db, stmt))
    return false;

  if (sqlite3_prepare_v2(db, "DELETE FROM memory_fts_grams WHERE rowid = ?",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "MemoryStore: fts grams delete prepare failed: "
             << sqlite3_errmsg(db);
    return false;
  }
  sqlite3_bind_int64(stmt, 1, id);
  if (!stepDone(db, stmt))
    return false;

  if (sqlite3_prepare_v2(db, "DELETE FROM memory_vec WHERE rowid = ?", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "MemoryStore: vec delete prepare failed: "
             << sqlite3_errmsg(db);
    return false;
  }
  sqlite3_bind_int64(stmt, 1, id);
  return stepDone(db, stmt);
}

std::vector<MemoryEntry> MemoryStore::list(const std::string& scope,
                                           int64_t refId, int limit)
{
  std::scoped_lock lock(VecDb::mutex());
  sqlite3* db = VecDb::handle();
  std::vector<MemoryEntry> out;
  if (!db)
    return out;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "SELECT id, scope, ref_id, type, content, priority, source, "
          "created_at, updated_at, hit_count FROM memory_l1 "
          "WHERE scope = ? AND ref_id = ? AND deleted_at IS NULL "
          "ORDER BY priority DESC, updated_at DESC LIMIT ?",
          -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "MemoryStore::list prepare failed: " << sqlite3_errmsg(db);
    return out;
  }
  sqlite3_bind_text(stmt, 1, scope.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, refId);
  sqlite3_bind_int(stmt, 3, limit);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char* rawScope =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const char* rawContent =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    const char* rawType =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    const char* rawSource =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    out.push_back({
        .id = sqlite3_column_int64(stmt, 0),
        .scope = rawScope ? rawScope : "",
        .refId = sqlite3_column_int64(stmt, 2),
        .type = rawType ? memoryTypeFromString(rawType) : MemoryType::Persona,
        .content = rawContent ? rawContent : "",
        .priority = sqlite3_column_int(stmt, 5),
        .source = rawSource ? rawSource : "",
        .createdAt = sqlite3_column_int64(stmt, 7),
        .updatedAt = sqlite3_column_int64(stmt, 8),
        .hitCount = sqlite3_column_int64(stmt, 9),
    });
  }
  sqlite3_finalize(stmt);
  return out;
}

size_t MemoryStore::count()
{
  std::scoped_lock lock(VecDb::mutex());
  sqlite3* db = VecDb::handle();
  if (!db)
    return 0;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db, "SELECT COUNT(*) FROM memory_l1 WHERE deleted_at IS NULL", -1,
          &stmt, nullptr) != SQLITE_OK) {
    LOG_WARN << "MemoryStore::count prepare failed: " << sqlite3_errmsg(db);
    return 0;
  }
  size_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
  sqlite3_finalize(stmt);
  return count;
}
