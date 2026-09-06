#include "vec-db.hxx"

#define SQLITE_CORE
#include "sqlite-vec.h"

#include <drogon/drogon.h>
#include <memory>
#include <shared/repositories/memory-graph/memory-graph-query.hxx>
#include <shared/repositories/vector-index/vector-index-repository.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/utils/schema-runner/schema-runner.hxx>
#include <sqlite3.h>
#include <string>

VecDb& VecDb::instance()
{
  static VecDb db;
  return db;
}

std::mutex& VecDb::mutex()
{
  return mutex_;
}

int VecDb::embeddingDims() const
{
  const int configured = ConfigService::getInt("memory.embedding_dim");
  return configured > 0 ? configured : 384;
}

bool VecDb::schemaOutdated()
{
  std::lock_guard lock(mutex_);
  sqlite3* db = handle();
  if (!db)
    return false;
  return !repo_.memoryVecMatches(db, embeddingDims());
}

void VecDb::recreateMemoryVecTable()
{
  std::lock_guard lock(mutex_);
  sqlite3* db = handle();
  if (db)
    repo_.recreateMemoryVec(db, embeddingDims());
}

void VecDb::recreateVecTables()
{
  std::lock_guard lock(mutex_);
  sqlite3* db = handle();
  if (db)
    repo_.recreateVecTables(db, embeddingDims());
}

void VecDb::applySchema()
{
  std::scoped_lock lock(mutex_);
  runSchemaFile(handle(), memory_graph_query::schemaFile());
}

sqlite3* VecDb::handle()
{
  if (db_)
    return db_.get();

  const std::string file = ConfigService::getString("database.file");
  sqlite3* raw = nullptr;
  if (sqlite3_open(file.c_str(), &raw) != SQLITE_OK) {
    LOG_ERROR << "VecDb: open failed: "
              << (raw ? sqlite3_errmsg(raw) : "no handle");
    if (raw)
      sqlite3_close(raw);
    return nullptr;
  }
  db_.reset(raw);

  sqlite3_busy_timeout(db_.get(), 5000);

  // vec0 must be registered on THIS connection: services open VecDb during
  // startup (before the beginning advice registers the auto-extension), so
  // relying on the global auto-extension leaves `face_vec`/`memory_vec`
  // silently unreadable.
  char* vecErr = nullptr;
  if (sqlite3_vec_init(db_.get(), &vecErr, nullptr) != SQLITE_OK) {
    LOG_ERROR << "VecDb: vec0 init failed: "
              << (vecErr ? vecErr : "unknown");
    if (vecErr)
      sqlite3_free(vecErr);
    db_.reset();
    return nullptr;
  }

  if (!repo_.createTables(db_.get(), embeddingDims())) {
    LOG_ERROR << "VecDb: vec tables setup failed";
    db_.reset();
    return nullptr;
  }

  if (!repo_.hasCosineMetric(db_.get()))
    repo_.recreateVecTables(db_.get(), embeddingDims());

  LOG_INFO << "VecDb: vec0 connection ready (dims=" << embeddingDims() << ")";
  return db_.get();
}
