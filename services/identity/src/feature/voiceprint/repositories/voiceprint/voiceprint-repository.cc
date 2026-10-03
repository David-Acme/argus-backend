#include "voiceprint-repository.hxx"

#include <ctime>
#include <drogon/drogon.h>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/sqlite-stmt.hxx>
#include <sqlite3.h>
#include <string_view>

using namespace voiceprint_query;

namespace
{

bool execute(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) == SQLITE_OK)
    return true;
  LOG_WARN << "VoiceprintRepository: " << (error ? error : "unknown error");
  sqlite3_free(error);
  return false;
}

std::string tableSql(sqlite3* db)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, VEC_TABLE_SQL) || stmt.step() != SQLITE_ROW)
    return {};
  return stmt.columnText(0);
}

struct BindVectorInput
{
  SqliteStmt& stmt;
  int index{0};
  std::span<const float> values;
};

bool bindVector(const BindVectorInput& input)
{
  return input.stmt.bindBlob({.index = input.index,
                              .data = input.values.data(),
                              .size = input.values.size() * sizeof(float)});
}

}

drogon::Task<std::optional<VoiceprintSchema>>
VoiceprintRepository::findByUser(int64_t userId,
                                 drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto rows = co_await resolved->execSqlCoro(FIND_BY_USER, userId);
  if (rows.empty())
    co_return std::nullopt;
  co_return VoiceprintSchema(rows.front());
}

drogon::Task<VoiceprintSchema>
VoiceprintRepository::create(const VoiceprintCreateInput& input) const
{
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result =
      co_await client->execSqlCoro(INSERT, input.userId, input.model,
                                   input.embedding, input.sampleCount,
                                   input.speechSeconds,
                                   voiceprintMethodToString(input.method),
                                   input.consentVersion, input.enrolledBy);

  VoiceprintSchema schema;
  schema.id = static_cast<int64_t>(result.insertId());
  schema.userId = input.userId;
  schema.model = input.model;
  schema.embedding = voice_vector::fromBlob(
      std::string_view(input.embedding.data(), input.embedding.size()));
  schema.sampleCount = input.sampleCount;
  schema.speechSeconds = input.speechSeconds;
  schema.method = input.method;
  schema.consentVersion = input.consentVersion;
  schema.enrolledBy = input.enrolledBy;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<int64_t>>
VoiceprintRepository::removeByUser(int64_t userId,
                                   drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto rows = co_await resolved->execSqlCoro(DELETE_BY_USER, userId);
  if (rows.empty())
    co_return std::nullopt;
  co_return rows.front()["id"].as<int64_t>();
}

std::vector<VoiceprintIndexRow>
VoiceprintRepository::findForModel(sqlite3* db, const std::string& model) const
{
  std::vector<VoiceprintIndexRow> rows;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_FOR_MODEL) || !stmt.bindText(1, model))
    return rows;
  while (stmt.step() == SQLITE_ROW) {
    const auto* blob =
        static_cast<const char*>(sqlite3_column_blob(stmt.get(), 2));
    const auto bytes = static_cast<size_t>(sqlite3_column_bytes(stmt.get(), 2));
    rows.push_back({.voiceprintId = stmt.columnInt64(0),
                    .userId = stmt.columnInt64(1),
                    .embedding = blob ? voice_vector::fromBlob(
                                            std::string_view(blob, bytes))
                                      : std::vector<float>{}});
  }
  return rows;
}

bool VoiceprintRepository::ensureVecTable(sqlite3* db, int dims) const
{
  const std::string current = tableSql(db);
  const std::string expected = "float[" + std::to_string(dims) + "]";
  if (!current.empty() && current.find(expected) != std::string::npos)
    return true;
  if (!current.empty() && !execute(db, VEC_DROP))
    return false;
  return execute(db, vecTableDdl(dims));
}

bool VoiceprintRepository::clearVec(sqlite3* db) const
{
  return execute(db, VEC_CLEAR);
}

bool VoiceprintRepository::insertVec(sqlite3* db,
                                     const VoiceprintVecEntry& entry) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, VEC_INSERT) || !stmt.bindInt64(1, entry.voiceprintId) ||
      !bindVector({.stmt = stmt, .index = 2, .values = entry.embedding}) ||
      !stmt.bindInt64(3, entry.userId))
    return false;
  return stmt.step() == SQLITE_DONE;
}

bool VoiceprintRepository::deleteVec(sqlite3* db, int64_t voiceprintId) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, VEC_DELETE) || !stmt.bindInt64(1, voiceprintId))
    return false;
  return stmt.step() == SQLITE_DONE;
}

std::vector<VoiceprintVecHit>
VoiceprintRepository::searchVec(sqlite3* db,
                                const VoiceprintVecSearchInput& input) const
{
  std::vector<VoiceprintVecHit> hits;
  SqliteStmt stmt;
  if (!stmt.prepare(db, VEC_SEARCH) ||
      !bindVector({.stmt = stmt, .index = 1, .values = input.query}) ||
      !stmt.bindInt(2, input.count))
    return hits;
  while (stmt.step() == SQLITE_ROW)
    hits.push_back({.voiceprintId = stmt.columnInt64(0),
                    .userId = stmt.columnInt64(1),
                    .distance = static_cast<float>(stmt.columnDouble(2))});
  return hits;
}
