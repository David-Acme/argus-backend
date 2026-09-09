#include <drogon/drogon.h>
#include <shared/repositories/vector-index/vector-index-query.hxx>
#include <shared/repositories/vector-index/vector-index-repository.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>

using namespace vector_index_query;

namespace
{

bool execUnlocked(sqlite3* db, const char* sql)
{
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    LOG_WARN << "VectorIndexRepository: " << (err ? err : "unknown") << " | "
             << sql;
    sqlite3_free(err);
    return false;
  }
  return true;
}

// The face index belongs to the legacy process only; the TOML boolean reads in both shapes.
bool createFaceVec()
{
  if (!ConfigService::hasKey("memory.create_face_vec"))
    return true;
  const std::string configured =
      ConfigService::getString("memory.create_face_vec");
  if (!configured.empty())
    return configured != "false";
  return ConfigService::getBool("memory.create_face_vec");
}

} // namespace

bool VectorIndexRepository::createTables(sqlite3* db, int dims)
{
  const std::string memory = memoryVecDdl(dims);
  if (!execUnlocked(db, memory.c_str()))
    return false;
  if (!createFaceVec())
    return true;
  return execUnlocked(db, FACE_VEC_DDL);
}

bool VectorIndexRepository::dropTables(sqlite3* db)
{
  return execUnlocked(db, DROP_MEMORY_VEC) && execUnlocked(db, DROP_FACE_VEC);
}

bool VectorIndexRepository::recreateMemoryVec(sqlite3* db, int dims)
{
  if (!execUnlocked(db, DROP_MEMORY_VEC))
    return false;
  const std::string memory = memoryVecDdl(dims);
  if (!execUnlocked(db, memory.c_str()))
    return false;
  LOG_WARN << "VectorIndexRepository: memory_vec recreated (schema change — "
              "memories must be re-embedded)";
  return true;
}

bool VectorIndexRepository::recreateVecTables(sqlite3* db, int dims)
{
  if (!dropTables(db))
    return false;
  if (!createTables(db, dims))
    return false;
  LOG_WARN << "VectorIndexRepository: vec tables recreated (L2 legacy "
              "dropped)";
  return true;
}

std::string VectorIndexRepository::tableSql(sqlite3* db,
                                            const std::string& name)
{
  SqliteStmt stmt;
  std::string out;
  if (!stmt.prepare(db, TABLE_SQL))
    return out;
  stmt.bindText(1, name);
  if (stmt.step() == SQLITE_ROW)
    out = stmt.columnText(0);
  return out;
}

bool VectorIndexRepository::hasCosineMetric(sqlite3* db)
{
  if (tableSql(db, "memory_vec").find("distance_metric=cosine") ==
      std::string::npos)
    return false;
  if (!createFaceVec())
    return true;
  return tableSql(db, "face_vec").find("distance_metric=cosine") !=
         std::string::npos;
}

bool VectorIndexRepository::memoryVecMatches(sqlite3* db, int dims)
{
  const std::string sql = tableSql(db, "memory_vec");
  if (sql.empty())
    return false;
  if (sql.find("float[" + std::to_string(dims) + "]") == std::string::npos)
    return false;
  if (sql.find("view INTEGER") == std::string::npos)
    return false;
  return true;
}
