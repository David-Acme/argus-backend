#include "memory-recall.hxx"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <drogon/drogon.h>
#include <map>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/embedding/embedding-service.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <sqlite3.h>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr int kRrfK = 60;
constexpr int kLayerLimit = 12;

struct Candidate
{
  int64_t id;
  float rrf = 0.0F;
  int priority = 50;
  int64_t createdAt = 0;
  int64_t hitCount = 0;
  std::string content;
};

std::vector<std::string> wordsOf(const std::string& text)
{
  std::vector<std::string> words;
  std::string current;
  for (unsigned char c : text) {
    if (std::isalnum(c)) {
      current += static_cast<char>(std::tolower(c));
    }
    else if (!current.empty()) {
      if (current.size() >= 3)
        words.push_back(current);
      current.clear();
    }
  }
  if (current.size() >= 3)
    words.push_back(current);
  return words;
}

std::string scopeClause(const std::vector<std::string>& scopes)
{
  std::string clause = "scope IN (";
  for (size_t i = 0; i < scopes.size(); ++i) {
    if (i > 0)
      clause += ",";
    clause += "?";
  }
  clause += ")";
  return clause;
}

void bindScopeFilter(sqlite3_stmt* stmt, int& index,
                     const std::vector<std::string>& scopes)
{
  for (const auto& scope : scopes)
    sqlite3_bind_text(stmt, index++, scope.data(), -1, SQLITE_TRANSIENT);
}

float recencyFactor(int64_t createdAt, int64_t now)
{
  const double days = static_cast<double>(now - createdAt) / 86400.0;
  return std::clamp(static_cast<float>(1.0 - days / 90.0), 0.3F, 1.0F);
}

int tokenBudgetChars(const std::string& key, int fallback)
{
  const int v = ConfigService::getInt(key);
  return (v > 0 ? v : fallback) * 4;
}

} // namespace

std::string MemoryRecall::profileText(
    int64_t userId, const std::vector<int64_t>& personIds)
{
  std::scoped_lock lock(VecDb::mutex());
  sqlite3* db = VecDb::handle();
  if (!db)
    return {};

  std::vector<std::string> parts;
  const int budget = tokenBudgetChars("memory.profile_max_tokens", 256);

  sqlite3_stmt* stmt = nullptr;
  const auto appendProfile = [&](const std::string& scope, int64_t refId) {
    if (sqlite3_prepare_v2(
            db,
            "SELECT content FROM memory_profile "
            "WHERE scope = ? AND ref_id = ? LIMIT 1",
            -1, &stmt, nullptr) != SQLITE_OK)
      return;
    sqlite3_bind_text(stmt, 1, scope.data(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, refId);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char* raw =
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      if (raw)
        parts.emplace_back(raw);
    }
    sqlite3_finalize(stmt);
  };

  appendProfile("global", 0);
  if (userId > 0)
    appendProfile("user", userId);
  for (int64_t pid : personIds)
    appendProfile("person", pid);

  std::string out;
  for (const auto& part : parts) {
    if (static_cast<int>(out.size()) + static_cast<int>(part.size()) > budget)
      break;
    if (!out.empty())
      out += "\n";
    out += part;
  }
  return out;
}

RecallContext MemoryRecall::recall(const RecallInput& input)
{
  RecallContext ctx;
  std::scoped_lock lock(VecDb::mutex());
  sqlite3* db = VecDb::handle();
  if (!db)
    return ctx;

  const std::string userScope = "user:" + std::to_string(input.userId);
  std::vector<std::string> partitions{"global"};
  if (input.userId > 0)
    partitions.push_back(userScope);
  for (int64_t pid : input.personIds)
    partitions.push_back("person:" + std::to_string(pid));

  const auto words = wordsOf(input.text);
  std::vector<std::vector<int64_t>> ranked;
  sqlite3_stmt* stmt = nullptr;

  if (!words.empty()) {
    std::string match;
    for (const auto& w : words) {
      if (!match.empty())
        match += " AND ";
      match += "\"" + w + "\"";
    }
    std::string sql =
        "SELECT rowid, bm25(memory_fts_words) AS r FROM memory_fts_words "
        "WHERE memory_fts_words MATCH ? AND " +
        scopeClause(partitions) + " ORDER BY r LIMIT " +
        std::to_string(kLayerLimit);
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
      int index = 1;
      sqlite3_bind_text(stmt, index++, match.data(), -1, SQLITE_TRANSIENT);
      bindScopeFilter(stmt, index, partitions);
      std::vector<int64_t> layer;
      while (sqlite3_step(stmt) == SQLITE_ROW)
        layer.push_back(sqlite3_column_int64(stmt, 0));
      sqlite3_finalize(stmt);
      ranked.push_back(std::move(layer));
    }
  }

  for (const auto& w : words) {
    std::string sql =
        "SELECT rowid, bm25(memory_fts_grams) AS r FROM memory_fts_grams "
        "WHERE memory_fts_grams MATCH ? AND " +
        scopeClause(partitions) + " ORDER BY r LIMIT " +
        std::to_string(kLayerLimit);
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
      int index = 1;
      sqlite3_bind_text(stmt, index++, w.data(), -1, SQLITE_TRANSIENT);
      bindScopeFilter(stmt, index, partitions);
      std::vector<int64_t> layer;
      while (sqlite3_step(stmt) == SQLITE_ROW)
        layer.push_back(sqlite3_column_int64(stmt, 0));
      sqlite3_finalize(stmt);
      ranked.push_back(std::move(layer));
    }
  }

  if (const auto vec =
          EmbeddingService::embed(input.text, "query:")) {
    for (const auto& partition : partitions) {
      std::string sql =
          "SELECT memory_id FROM memory_vec "
          "WHERE embedding MATCH ? AND partition = ? "
          "ORDER BY distance LIMIT " +
          std::to_string(kLayerLimit);
      if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        std::string enc = "[";
        for (size_t i = 0; i < vec->size(); ++i) {
          if (i > 0)
            enc += ",";
          enc += std::to_string((*vec)[i]);
        }
        enc += "]";
        int index = 1;
        sqlite3_bind_text(stmt, index++, enc.data(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, index++, partition.data(), -1,
                          SQLITE_TRANSIENT);
        std::vector<int64_t> layer;
        while (sqlite3_step(stmt) == SQLITE_ROW)
          layer.push_back(sqlite3_column_int64(stmt, 0));
        sqlite3_finalize(stmt);
        ranked.push_back(std::move(layer));
      }
    }
  }

  std::map<int64_t, Candidate> candidates;
  for (const auto& layer : ranked) {
    for (size_t pos = 0; pos < layer.size(); ++pos) {
      auto& cand = candidates[layer[pos]];
      cand.id = layer[pos];
      cand.rrf += 1.0F / static_cast<float>(kRrfK + pos + 1);
    }
  }

  if (candidates.empty())
    return ctx;

  const std::string idsSql = [&] {
    std::string sql = "SELECT id, content, priority, created_at, hit_count "
                      "FROM memory_l1 WHERE id IN (";
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (i > 0)
        sql += ",";
      sql += "?";
    }
    sql += ") AND deleted_at IS NULL";
    return sql;
  }();

  if (sqlite3_prepare_v2(db, idsSql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
    int index = 1;
    for (const auto& [id, cand] : candidates)
      sqlite3_bind_int64(stmt, index++, id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const int64_t id = sqlite3_column_int64(stmt, 0);
      const char* raw =
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      auto& cand = candidates[id];
      cand.content = raw ? raw : "";
      cand.priority = sqlite3_column_int(stmt, 2);
      cand.createdAt = sqlite3_column_int64(stmt, 3);
      cand.hitCount = sqlite3_column_int64(stmt, 4);
    }
    sqlite3_finalize(stmt);
  }

  const int64_t now = std::time(nullptr);
  const int topK = ConfigService::getInt("memory.recall_top_k");
  std::vector<Candidate> ordered;
  for (const auto& [id, cand] : candidates) {
    if (cand.content.empty())
      continue;
    Candidate scored = cand;
    scored.rrf *= (0.5F + static_cast<float>(cand.priority) / 100.0F) *
                  (1.0F + std::log1p(static_cast<float>(cand.hitCount))) *
                  recencyFactor(cand.createdAt, now);
    ordered.push_back(std::move(scored));
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const Candidate& a, const Candidate& b) { return a.rrf > b.rrf; });
  if (static_cast<int>(ordered.size()) > std::max(1, topK))
    ordered.resize(static_cast<size_t>(std::max(1, topK)));

  const int budget = tokenBudgetChars("memory.recall_max_tokens", 512);
  std::string body;
  for (const auto& cand : ordered) {
    const int cost = static_cast<int>(cand.content.size()) + 4;
    if (!body.empty() &&
        static_cast<int>(body.size()) + cost > budget)
      break;
    body += "- " + cand.content + "\n";
    ctx.usedIds.push_back(cand.id);
  }
  if (body.empty())
    return ctx;

  const std::string header =
      input.lang == "es"
          ? "<relevant-memories>\nMemorias relevantes para responder (usa su "
            "contenido como contexto real):\n"
          : "<relevant-memories>\nRelevant memories for answering (use their "
            "content as real context):\n";
  const std::string footer = "</relevant-memories>";
  ctx.prependText = header + body + footer;
  return ctx;
}
