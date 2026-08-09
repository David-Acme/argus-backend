#include "vec-db.hxx"

#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>
#include <sqlite3.h>
#include <string>

namespace
{

std::mutex gMutex;
sqlite3* gDb = nullptr;

constexpr const char* kCreateTables =
    "CREATE VIRTUAL TABLE IF NOT EXISTS memory_vec USING vec0("
    "embedding float[384], partition TEXT PARTITION KEY, memory_id INTEGER);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS face_vec USING vec0("
    "embedding float[128], person_id INTEGER, face_embedding_id INTEGER);";

} // namespace

std::mutex& VecDb::mutex()
{
  return gMutex;
}

sqlite3* VecDb::handle()
{
  if (gDb)
    return gDb;

  const std::string file = ConfigService::getString("database.file");
  if (sqlite3_open(file.c_str(), &gDb) != SQLITE_OK) {
    LOG_ERROR << "VecDb: open failed: "
              << (gDb ? sqlite3_errmsg(gDb) : "no handle");
    if (gDb) {
      sqlite3_close(gDb);
      gDb = nullptr;
    }
    return nullptr;
  }

  sqlite3_busy_timeout(gDb, 5000);

  char* err = nullptr;
  if (sqlite3_exec(gDb, kCreateTables, nullptr, nullptr, &err) != SQLITE_OK) {
    LOG_ERROR << "VecDb: vec tables setup failed: " << (err ? err : "unknown");
    sqlite3_free(err);
    sqlite3_close(gDb);
    gDb = nullptr;
    return nullptr;
  }

  LOG_INFO << "VecDb: vec0 connection ready";
  return gDb;
}
