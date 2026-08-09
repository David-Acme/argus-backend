#include "memory-service.hxx"

#include <drogon/drogon.h>
#include <shared/services/embedding/embedding-service.hxx>
#include <shared/services/memory/memory-recall.hxx>
#include <shared/services/memory/memory-store.hxx>
#include <shared/services/memory/rule-parser.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace
{

std::string partitionOf(MemoryScope scope, int64_t refId)
{
  if (scope == MemoryScope::Global)
    return "global";
  return memoryScopeToString(scope) + ":" + std::to_string(refId);
}

void insertVector(int64_t memoryId, const std::string& content,
                  MemoryScope scope, int64_t refId)
{
  const auto vec = EmbeddingService::embed(content, "passage:");
  if (!vec)
    return;

  std::scoped_lock lock(VecDb::mutex());
  sqlite3* db = VecDb::handle();
  if (!db)
    return;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "INSERT INTO memory_vec (rowid, embedding, partition, memory_id) "
          "VALUES (?, ?, ?, ?)",
          -1, &stmt, nullptr) != SQLITE_OK)
    return;

  std::string enc = "[";
  for (size_t i = 0; i < vec->size(); ++i) {
    if (i > 0)
      enc += ",";
    enc += std::to_string((*vec)[i]);
  }
  enc += "]";

  const std::string partition = partitionOf(scope, refId);
  sqlite3_bind_int64(stmt, 1, memoryId);
  sqlite3_bind_text(stmt, 2, enc.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, partition.data(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, memoryId);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

} // namespace

void MemoryService::init()
{
  EmbeddingService::init();
}

void MemoryService::shutdown()
{
  EmbeddingService::shutdown();
}

bool MemoryService::isLoaded()
{
  return EmbeddingService::isLoaded();
}

int64_t MemoryService::captureExplicit(const CaptureInput& input)
{
  const auto parsed = RuleParser::parse({.text = input.text, .lang = input.lang});
  if (!parsed)
    return -1;

  const int64_t id = MemoryStore::saveDedup({
      .scope = MemoryScope::User,
      .refId = input.userId,
      .type = parsed->type,
      .content = parsed->content,
      .priority = parsed->priority,
      .source = MemorySource::Rule,
      .sourceTurnId = std::nullopt,
      .lang = input.lang,
  });
  if (id > 0)
    insertVector(id, parsed->content, MemoryScope::User, input.userId);
  return id;
}

int64_t MemoryService::captureToolCall(int64_t userId, const std::string& lang,
                                       const ToolCall& call)
{
  const int64_t id = MemoryStore::saveDedup({
      .scope = MemoryScope::User,
      .refId = userId,
      .type = call.type,
      .content = call.content,
      .priority = call.priority,
      .source = MemorySource::Llm,
      .sourceTurnId = std::nullopt,
      .lang = lang,
  });
  if (id > 0)
    insertVector(id, call.content, MemoryScope::User, userId);
  return id;
}

RecallContext MemoryService::recall(const RecallInput& input)
{
  RecallContext ctx = MemoryRecall::recall(input);
  ctx.profileText = MemoryRecall::profileText(input.userId, input.personIds);
  return ctx;
}

void MemoryService::bumpHitCount(const std::vector<int64_t>& ids)
{
  if (ids.empty())
    return;

  std::scoped_lock lock(VecDb::mutex());
  sqlite3* db = VecDb::handle();
  if (!db)
    return;

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "UPDATE memory_l1 SET hit_count = hit_count + 1 WHERE id = ?",
          -1, &stmt, nullptr) != SQLITE_OK)
    return;
  for (int64_t id : ids) {
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);
}
