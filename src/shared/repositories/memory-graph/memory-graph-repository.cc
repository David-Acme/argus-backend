#include "memory-graph-repository.hxx"

#include <ctime>
#include <drogon/drogon.h>
#include <shared/repositories/memory-graph/memory-graph-query.hxx>
#include <shared/utils/text-norm/text-norm.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>
#include <utility>

using namespace memory_graph_query;

namespace
{

int64_t lastRowId(sqlite3* db)
{
  return sqlite3_last_insert_rowid(db);
}

} // namespace

int64_t MemoryGraphRepository::createEntity(sqlite3* db,
                                            const EntityCreateInput& input)
{
  if (!db)
    return 0;
  const int64_t now = std::time(nullptr);
  SqliteStmt stmt;
  if (!stmt.prepare(db, INSERT_ENTITY))
    return 0;
  stmt.bindText(1, input.kind);
  stmt.bindText(2, input.canonical);
  stmt.bindText(3, input.lang);
  stmt.bindInt64(4, now);
  stmt.bindInt64(5, now);
  if (input.personId)
    stmt.bindInt64(6, *input.personId);
  else
    stmt.bindNull(6);
  if (stmt.step() != SQLITE_DONE)
    return 0;
  return lastRowId(db);
}

std::optional<int64_t>
MemoryGraphRepository::resolveEntity(sqlite3* db,
                                     const AliasResolveInput& input)
{
  if (!db)
    return std::nullopt;
  SqliteStmt stmt;
  if (stmt.prepare(db, FIND_ENTITY_BY_NORM)) {
    stmt.bindText(1, input.norm);
    stmt.bindText(2, input.lang);
    if (stmt.step() == SQLITE_ROW)
      return stmt.columnInt64(0);
  }
  const auto tokens = text_norm::words(input.norm);
  if (!tokens.empty()) {
    std::string match;
    for (const auto& token : tokens) {
      if (!match.empty())
        match += " OR ";
      match += "\"" + token + "\"";
    }
    if (stmt.prepare(db, FIND_ENTITY_BY_FTS)) {
      stmt.bindText(1, match);
      if (stmt.step() == SQLITE_ROW)
        return stmt.columnInt64(0);
    }
  }
  return std::nullopt;
}

int64_t MemoryGraphRepository::addAlias(sqlite3* db,
                                        const AliasCreateInput& input)
{
  if (!db)
    return 0;
  SqliteStmt stmt;
  if (stmt.prepare(db, FIND_ALIAS)) {
    stmt.bindInt64(1, input.entityId);
    stmt.bindText(2, input.norm);
    if (stmt.step() == SQLITE_ROW)
      return stmt.columnInt64(0);
  }
  if (!stmt.prepare(db, INSERT_ALIAS))
    return 0;
  stmt.bindInt64(1, input.entityId);
  stmt.bindText(2, input.surface);
  stmt.bindText(3, input.norm);
  stmt.bindText(4, input.lang);
  stmt.bindText(5, input.personFrame);
  stmt.bindDouble(6, input.confidence);
  if (stmt.step() != SQLITE_DONE)
    return 0;
  const int64_t id = lastRowId(db);
  ftsInsert(db, "memory_alias_fts", "norm", id, input.norm);
  return id;
}

std::vector<AliasInfo> MemoryGraphRepository::aliasesForEntity(sqlite3* db,
                                                               int64_t entityId)
{
  std::vector<AliasInfo> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_ALIASES))
    return out;
  stmt.bindInt64(1, entityId);
  while (stmt.step() == SQLITE_ROW)
    out.push_back(
        {.surface = stmt.columnText(0), .personFrame = stmt.columnText(1)});
  return out;
}

int64_t MemoryGraphRepository::upsertFact(sqlite3* db,
                                          const FactUpsertInput& input)
{
  if (!db)
    return 0;
  SqliteStmt stmt;
  int64_t oldId = 0;
  if (stmt.prepare(db, FIND_OPEN_FACT)) {
    stmt.bindInt64(1, input.entityId);
    stmt.bindText(2, input.predicate);
    stmt.bindText(3, input.scope);
    stmt.bindInt64(4, input.refId);
    if (stmt.step() == SQLITE_ROW)
      oldId = stmt.columnInt64(0);
  }
  if (!stmt.prepare(db, INSERT_FACT))
    return 0;
  stmt.bindInt64(1, input.entityId);
  stmt.bindText(2, input.predicate);
  stmt.bindText(3, input.value);
  stmt.bindText(4, input.canonical);
  stmt.bindText(5, input.type);
  stmt.bindInt(6, input.priority);
  stmt.bindDouble(7, input.confidence);
  stmt.bindText(8, input.lang);
  stmt.bindText(9, input.scope);
  stmt.bindInt64(10, input.refId);
  stmt.bindInt64(11, input.now);
  stmt.bindInt64(12, input.now);
  stmt.bindInt64(13, input.now);
  if (stmt.step() != SQLITE_DONE)
    return 0;
  const int64_t newId = lastRowId(db);

  if (oldId > 0) {
    if (stmt.prepare(db, CLOSE_FACT)) {
      stmt.bindInt64(1, input.now);
      stmt.bindInt64(2, input.now);
      stmt.bindInt64(3, oldId);
      stmt.step();
    }
    insertEdge(db, "supersedes", oldId, newId, "", input.now, 0, 0);
  }
  if (input.sourceId)
    insertEdge(db, "derived", newId, *input.sourceId, "", 0, 0, 0);
  ftsInsert(db, "memory_fact_fts", "canonical", newId, input.canonical);
  return newId;
}

bool MemoryGraphRepository::closeFact(sqlite3* db, int64_t factId, int64_t at)
{
  if (!db)
    return false;
  SqliteStmt stmt;
  if (!stmt.prepare(db, CLOSE_FACT))
    return false;
  stmt.bindInt64(1, at);
  stmt.bindInt64(2, at);
  stmt.bindInt64(3, factId);
  return stmt.step() == SQLITE_DONE;
}

std::vector<RecallHit>
MemoryGraphRepository::factsForEntity(sqlite3* db,
                                      const RecallEntityInput& input)
{
  std::vector<RecallHit> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_FACTS_BY_ENTITY))
    return out;
  stmt.bindInt64(1, input.entityId);
  stmt.bindInt(2, input.maxHops);
  stmt.bindText(3, input.scope);
  stmt.bindInt64(4, input.refId);
  stmt.bindInt(5, input.limit);
  while (stmt.step() == SQLITE_ROW) {
    RecallHit hit;
    hit.factId = stmt.columnInt64(0);
    hit.entityId = stmt.columnInt64(1);
    hit.predicate = stmt.columnText(2);
    hit.value = stmt.columnText(3);
    hit.canonical = stmt.columnText(4);
    hit.type = stmt.columnText(5);
    hit.priority = stmt.columnInt(6);
    hit.confidence = static_cast<float>(stmt.columnDouble(7));
    hit.hitCount = stmt.columnInt64(8);
    hit.hops = static_cast<int>(stmt.columnInt64(9));
    hit.score =
        static_cast<float>((input.maxHops - hit.hops + 1) * 100 + hit.priority);
    out.push_back(std::move(hit));
  }
  return out;
}

std::vector<RecallHit> MemoryGraphRepository::ftsFacts(sqlite3* db,
                                                       const std::string& match,
                                                       const std::string& scope,
                                                       int64_t refId, int limit)
{
  std::vector<RecallHit> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_FACTS_FTS))
    return out;
  stmt.bindText(1, match);
  stmt.bindText(2, scope);
  stmt.bindInt64(3, refId);
  stmt.bindInt(4, limit);
  while (stmt.step() == SQLITE_ROW) {
    RecallHit hit;
    hit.factId = stmt.columnInt64(0);
    hit.entityId = stmt.columnInt64(1);
    hit.predicate = stmt.columnText(2);
    hit.value = stmt.columnText(3);
    hit.canonical = stmt.columnText(4);
    hit.type = stmt.columnText(5);
    hit.priority = stmt.columnInt(6);
    hit.confidence = static_cast<float>(stmt.columnDouble(7));
    hit.hitCount = stmt.columnInt64(8);
    hit.hops = 1;
    hit.score = static_cast<float>(hit.priority);
    out.push_back(std::move(hit));
  }
  return out;
}

std::vector<VecNeighbour>
MemoryGraphRepository::vecNeighbours(sqlite3* db,
                                     const VecNeighbourInput& input)
{
  std::vector<VecNeighbour> out;
  if (!db || input.k <= 0)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_VEC_NEIGHBOURS))
    return out;
  stmt.bindText(1, input.encoded);
  stmt.bindText(2, input.partition);
  stmt.bindInt(3, input.k);
  while (stmt.step() == SQLITE_ROW)
    out.push_back({.factId = stmt.columnInt64(0),
                   .distance = static_cast<float>(stmt.columnDouble(1))});
  return out;
}

std::optional<RecallHit>
MemoryGraphRepository::factById(sqlite3* db, const FactByIdInput& input)
{
  if (!db || input.factId <= 0)
    return std::nullopt;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_FACT_BY_ID))
    return std::nullopt;
  stmt.bindInt64(1, input.factId);
  stmt.bindText(2, input.scope);
  stmt.bindInt64(3, input.refId);
  if (stmt.step() != SQLITE_ROW)
    return std::nullopt;
  RecallHit hit;
  hit.factId = stmt.columnInt64(0);
  hit.entityId = stmt.columnInt64(1);
  hit.predicate = stmt.columnText(2);
  hit.value = stmt.columnText(3);
  hit.canonical = stmt.columnText(4);
  hit.type = stmt.columnText(5);
  hit.priority = stmt.columnInt(6);
  hit.confidence = static_cast<float>(stmt.columnDouble(7));
  hit.hitCount = stmt.columnInt64(8);
  hit.hops = 1;
  hit.score = static_cast<float>(hit.priority);
  return hit;
}

int64_t MemoryGraphRepository::recordEpisode(sqlite3* db,
                                             const EpisodeCreateInput& input)
{
  if (!db)
    return 0;
  SqliteStmt stmt;
  if (!stmt.prepare(db, INSERT_EPISODE))
    return 0;
  stmt.bindText(1, input.kind);
  stmt.bindText(2, input.summary);
  if (input.actor.empty())
    stmt.bindNull(3);
  else
    stmt.bindText(3, input.actor);
  stmt.bindInt64(4, input.occurredAt);
  if (input.sessionId.empty())
    stmt.bindNull(5);
  else
    stmt.bindText(5, input.sessionId);
  stmt.bindText(6, input.lang);
  stmt.bindText(7, input.scope);
  stmt.bindInt64(8, input.refId);
  stmt.bindDouble(9, input.salience);
  if (stmt.step() != SQLITE_DONE)
    return 0;
  const int64_t id = lastRowId(db);

  if (input.sourceId)
    insertEdge(db, "derived", id, *input.sourceId, "", 0, 0, 0);
  for (int64_t entityId : input.mentionEntityIds)
    insertEdge(db, "mentions", id, entityId, "", 0, 0, 0);
  ftsInsert(db, "memory_episode_fts", "summary", id, input.summary);
  return id;
}

std::vector<int64_t>
MemoryGraphRepository::episodesBetween(sqlite3* db, const std::string& scope,
                                       int64_t refId, int64_t from, int64_t to,
                                       int limit)
{
  std::vector<int64_t> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_EPISODES_BETWEEN))
    return out;
  stmt.bindText(1, scope);
  stmt.bindInt64(2, refId);
  stmt.bindInt64(3, from);
  stmt.bindInt64(4, to);
  stmt.bindInt(5, limit);
  while (stmt.step() == SQLITE_ROW)
    out.push_back(stmt.columnInt64(0));
  return out;
}

int64_t MemoryGraphRepository::createSource(sqlite3* db,
                                            const std::string& channel,
                                            const std::string& turnRef,
                                            int64_t at)
{
  if (!db)
    return 0;
  SqliteStmt stmt;
  if (!stmt.prepare(db, INSERT_SOURCE))
    return 0;
  stmt.bindText(1, channel);
  if (turnRef.empty())
    stmt.bindNull(2);
  else
    stmt.bindText(2, turnRef);
  stmt.bindInt64(3, at);
  if (stmt.step() != SQLITE_DONE)
    return 0;
  return lastRowId(db);
}

void MemoryGraphRepository::bumpFactHits(sqlite3* db,
                                         const std::vector<int64_t>& factIds)
{
  if (factIds.empty() || !db)
    return;
  SqliteStmt stmt;
  if (!stmt.prepare(db, BUMP_FACT_HITS))
    return;
  for (int64_t id : factIds) {
    stmt.bindInt64(1, id);
    stmt.step();
    stmt.reset();
  }
}

int64_t MemoryGraphRepository::recordProcedure(sqlite3* db,
                                               const std::string& name,
                                               const std::string& goal,
                                               const std::string& steps)
{
  if (!db || name.empty() || steps.empty())
    return 0;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_PROCEDURE))
    return 0;
  stmt.bindText(1, name);
  if (stmt.step() == SQLITE_ROW) {
    const int64_t id = stmt.columnInt64(0);
    SqliteStmt upd;
    if (upd.prepare(db, BUMP_PROCEDURE)) {
      upd.bindInt64(1, std::time(nullptr));
      upd.bindInt64(2, id);
      upd.step();
    }
    return id;
  }
  if (!stmt.prepare(db, INSERT_PROCEDURE))
    return 0;
  stmt.bindText(1, name);
  stmt.bindText(2, goal);
  stmt.bindText(3, steps);
  stmt.bindInt64(4, std::time(nullptr));
  if (stmt.step() != SQLITE_DONE)
    return 0;
  return lastRowId(db);
}

std::optional<std::string>
MemoryGraphRepository::findProcedure(sqlite3* db, const std::string& goal)
{
  if (!db || goal.empty())
    return std::nullopt;
  const auto tokens = text_norm::words(goal);
  if (tokens.empty())
    return std::nullopt;
  std::string match;
  for (const auto& token : tokens) {
    if (!match.empty())
      match += " OR ";
    match += "\"" + token + "\"";
  }
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_PROCEDURE_STEPS))
    return std::nullopt;
  stmt.bindText(1, match);
  if (stmt.step() == SQLITE_ROW)
    return stmt.columnText(0);
  return std::nullopt;
}

void MemoryGraphRepository::ftsInsert(sqlite3* db, const char* table,
                                      const char* column, int64_t rowid,
                                      const std::string& text)
{
  if (!db)
    return;
  SqliteStmt stmt;
  const std::string sql = std::string("INSERT INTO ") + table + " (rowid, " +
                          column + ") VALUES (?, ?)";
  if (!stmt.prepare(db, sql.c_str()))
    return;
  stmt.bindInt64(1, rowid);
  stmt.bindText(2, text);
  stmt.step();
}

void MemoryGraphRepository::insertEdge(sqlite3* db, const std::string& kind,
                                       int64_t src, int64_t dst,
                                       const std::string& predicate,
                                       int64_t since, int64_t until, int ord)
{
  if (!db)
    return;
  SqliteStmt stmt;
  if (!stmt.prepare(db, INSERT_EDGE))
    return;
  stmt.bindText(1, kind);
  stmt.bindInt64(2, src);
  stmt.bindInt64(3, dst);
  if (predicate.empty())
    stmt.bindNull(4);
  else
    stmt.bindText(4, predicate);
  stmt.bindInt64(5, since);
  stmt.bindInt64(6, until);
  stmt.bindInt(7, ord);
  stmt.step();
}

std::optional<std::string>
MemoryGraphRepository::factContent(sqlite3* db, int64_t factId,
                                   std::string& scope, int64_t& refId)
{
  if (!db)
    return std::nullopt;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_FACT_CONTENT))
    return std::nullopt;
  stmt.bindInt64(1, factId);
  if (stmt.step() != SQLITE_ROW)
    return std::nullopt;
  scope = stmt.columnText(0);
  refId = stmt.columnInt64(1);
  return stmt.columnText(2);
}

std::optional<EpisodeHit>
MemoryGraphRepository::episodeById(sqlite3* db, const EpisodeByIdInput& input)
{
  if (!db || input.episodeId <= 0)
    return std::nullopt;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_EPISODE_BY_ID))
    return std::nullopt;
  stmt.bindInt64(1, input.episodeId);
  stmt.bindText(2, input.scope);
  stmt.bindInt64(3, input.refId);
  if (stmt.step() != SQLITE_ROW)
    return std::nullopt;
  EpisodeHit hit;
  hit.episodeId = stmt.columnInt64(0);
  hit.summary = stmt.columnText(1);
  hit.salience = static_cast<float>(stmt.columnDouble(2));
  hit.hitCount = stmt.columnInt64(3);
  hit.occurredAt = stmt.columnInt64(4);
  return hit;
}

std::vector<EpisodeHit>
MemoryGraphRepository::ftsEpisodes(sqlite3* db, const std::string& match,
                                   const std::string& scope, int64_t refId,
                                   int limit)
{
  std::vector<EpisodeHit> out;
  if (!db || match.empty())
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_EPISODES_FTS))
    return out;
  stmt.bindText(1, match);
  stmt.bindText(2, scope);
  stmt.bindInt64(3, refId);
  stmt.bindInt(4, limit);
  while (stmt.step() == SQLITE_ROW) {
    EpisodeHit hit;
    hit.episodeId = stmt.columnInt64(0);
    hit.summary = stmt.columnText(1);
    hit.salience = static_cast<float>(stmt.columnDouble(2));
    hit.hitCount = stmt.columnInt64(3);
    hit.occurredAt = stmt.columnInt64(4);
    out.push_back(std::move(hit));
  }
  return out;
}

std::optional<std::string>
MemoryGraphRepository::episodeContent(sqlite3* db, int64_t episodeId,
                                      std::string& scope, int64_t& refId)
{
  if (!db)
    return std::nullopt;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_EPISODE_CONTENT))
    return std::nullopt;
  stmt.bindInt64(1, episodeId);
  if (stmt.step() != SQLITE_ROW)
    return std::nullopt;
  scope = stmt.columnText(0);
  refId = stmt.columnInt64(1);
  return stmt.columnText(2);
}

void MemoryGraphRepository::bumpEpisodeHits(sqlite3* db,
                                            const std::vector<int64_t>& ids,
                                            int64_t at)
{
  if (!db || ids.empty())
    return;
  SqliteStmt stmt;
  if (!stmt.prepare(db, BUMP_EPISODE_HITS))
    return;
  for (const int64_t id : ids) {
    stmt.bindInt64(1, at);
    stmt.bindInt64(2, id);
    stmt.step();
    stmt.reset();
  }
}

void MemoryGraphRepository::bumpFactImportance(sqlite3* db, int64_t factId,
                                               int64_t at)
{
  if (!db || factId <= 0)
    return;
  SqliteStmt stmt;
  if (!stmt.prepare(db, BUMP_FACT_IMPORTANCE))
    return;
  stmt.bindInt64(1, at);
  stmt.bindInt64(2, factId);
  stmt.step();
}

std::vector<ProfileFactRow>
MemoryGraphRepository::topProfileFacts(sqlite3* db, int64_t refId, int limit)
{
  std::vector<ProfileFactRow> out;
  if (!db || limit <= 0)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_PROFILE_FACTS))
    return out;
  stmt.bindText(1, "user");
  stmt.bindInt64(2, refId);
  stmt.bindInt(3, limit);
  while (stmt.step() == SQLITE_ROW) {
    ProfileFactRow row;
    row.canonical = stmt.columnText(0);
    row.type = stmt.columnText(1);
    out.push_back(std::move(row));
  }
  return out;
}

std::vector<int64_t> MemoryGraphRepository::openFactIds(sqlite3* db)
{
  std::vector<int64_t> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_OPEN_FACT_IDS))
    return out;
  while (stmt.step() == SQLITE_ROW)
    out.push_back(stmt.columnInt64(0));
  return out;
}

void MemoryGraphRepository::insertVecRow(sqlite3* db,
                                         const std::string& partition,
                                         int64_t factId, int view,
                                         const std::vector<float>& vec)
{
  if (!db)
    return;
  std::string enc = "[";
  for (size_t i = 0; i < vec.size(); ++i) {
    if (i > 0)
      enc += ",";
    enc += std::to_string(vec[i]);
  }
  enc += "]";
  SqliteStmt stmt;
  if (!stmt.prepare(db, INSERT_VEC_ROW))
    return;
  stmt.bindText(1, enc);
  stmt.bindText(2, partition);
  stmt.bindInt64(3, factId);
  stmt.bindInt(4, view);
  stmt.step();
}

float MemoryGraphRepository::vecDedupSim(sqlite3* db,
                                         const std::string& encoded,
                                         const std::string& partition,
                                         int64_t factId)
{
  if (!db)
    return 0.0F;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_VEC_DUP))
    return 0.0F;
  stmt.bindText(1, encoded);
  stmt.bindText(2, partition);
  stmt.bindInt64(3, factId);
  if (stmt.step() == SQLITE_ROW)
    return 1.0F - static_cast<float>(stmt.columnDouble(1));
  return 0.0F;
}

void MemoryGraphRepository::deleteVecRows(sqlite3* db, int64_t factId)
{
  if (!db)
    return;
  SqliteStmt stmt;
  if (!stmt.prepare(db, DELETE_VEC_ROWS))
    return;
  stmt.bindInt64(1, factId);
  stmt.step();
}

std::vector<GazetteerRow> MemoryGraphRepository::gazetteerAliases(sqlite3* db)
{
  std::vector<GazetteerRow> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_ALIAS_GAZETTEER))
    return out;
  while (stmt.step() == SQLITE_ROW) {
    out.push_back({.norm = stmt.columnText(0),
                   .surface = stmt.columnText(1),
                   .personFrame = stmt.columnText(2),
                   .entityId = stmt.columnInt64(3),
                   .kind = stmt.columnText(4)});
  }
  return out;
}

std::vector<CatalogRow> MemoryGraphRepository::catalogPersons(sqlite3* db)
{
  std::vector<CatalogRow> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_PERSONS))
    return out;
  while (stmt.step() == SQLITE_ROW) {
    const int64_t id = stmt.columnInt64(0);
    for (const std::string& surface :
         {stmt.columnText(1), stmt.columnText(2)}) {
      const std::string norm = text_norm::whitespace(surface);
      if (!norm.empty())
        out.push_back({.surface = surface, .kind = "person", .catalogId = id});
    }
  }
  return out;
}

std::vector<CatalogRow> MemoryGraphRepository::catalogCameras(sqlite3* db)
{
  std::vector<CatalogRow> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_CAMERAS))
    return out;
  while (stmt.step() == SQLITE_ROW) {
    const std::string norm = text_norm::whitespace(stmt.columnText(1));
    if (!norm.empty())
      out.push_back({.surface = stmt.columnText(1),
                     .kind = "device",
                     .catalogId = stmt.columnInt64(0)});
  }
  return out;
}

std::vector<CatalogRow> MemoryGraphRepository::catalogZones(sqlite3* db)
{
  std::vector<CatalogRow> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_ZONES))
    return out;
  while (stmt.step() == SQLITE_ROW) {
    const std::string norm = text_norm::whitespace(stmt.columnText(1));
    if (!norm.empty())
      out.push_back({.surface = stmt.columnText(1),
                     .kind = "place",
                     .catalogId = stmt.columnInt64(0)});
  }
  return out;
}

std::vector<CatalogRow> MemoryGraphRepository::catalogStreams(sqlite3* db)
{
  std::vector<CatalogRow> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_STREAMS))
    return out;
  while (stmt.step() == SQLITE_ROW) {
    const std::string norm = text_norm::whitespace(stmt.columnText(1));
    if (!norm.empty())
      out.push_back({.surface = stmt.columnText(1),
                     .kind = "device",
                     .catalogId = stmt.columnInt64(0)});
  }
  return out;
}

int64_t MemoryGraphRepository::migrateLegacy(sqlite3* db)
{
  if (!db)
    return 0;
  SqliteStmt stmt;
  if (stmt.prepare(db, FIND_LEGACY_ENTITY) && stmt.step() == SQLITE_ROW)
    return 0;
  if (stmt.prepare(db, HAS_LEGACY_TABLE) && stmt.step() == SQLITE_ROW &&
      stmt.columnInt64(0) == 0)
    return 0;

  const int64_t now = std::time(nullptr);
  int64_t legacyEntity = 0;
  if (stmt.prepare(db, INSERT_LEGACY_ENTITY)) {
    stmt.bindInt64(1, now);
    stmt.bindInt64(2, now);
    if (stmt.step() == SQLITE_DONE)
      legacyEntity = lastRowId(db);
  }
  if (legacyEntity == 0)
    return 0;

  int64_t migrated = 0;
  if (stmt.prepare(db, FIND_LEGACY_ROWS)) {
    while (stmt.step() == SQLITE_ROW) {
      const std::string content = stmt.columnText(4);
      const std::string typeRaw = stmt.columnText(3);
      std::string factType = "attribute";
      if (typeRaw == "persona")
        factType = "persona";
      else if (typeRaw == "instruction")
        factType = "instruction";
      SqliteStmt ins;
      if (!ins.prepare(db, INSERT_LEGACY_FACT))
        continue;
      ins.bindInt64(1, legacyEntity);
      ins.bindText(2, content);
      ins.bindText(3, content);
      ins.bindText(4, factType);
      ins.bindInt(5, stmt.columnInt(5));
      ins.bindText(6, stmt.columnText(1));
      ins.bindInt64(7, stmt.columnInt64(2));
      ins.bindInt64(8, stmt.columnInt64(7));
      ins.bindInt64(9, stmt.columnInt64(6));
      ins.bindInt64(10, stmt.columnInt64(7));
      ins.bindInt64(11, stmt.columnInt64(8));
      if (ins.step() != SQLITE_DONE)
        continue;
      const int64_t factId = lastRowId(db);
      ftsInsert(db, "memory_fact_fts", "canonical", factId, content);
      ++migrated;
    }
  }
  return migrated;
}
