#include "sqlite-graph.hxx"

#include <ctime>
#include <drogon/drogon.h>
#include <shared/repositories/memory-graph/memory-graph-repository.hxx>
#include <shared/utils/schema-runner/schema-runner.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>
#include <utility>
#include <vector>

namespace
{

bool execUnlocked(sqlite3* db, const char* sql)
{
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    LOG_WARN << "SqliteGraph: " << (err ? err : "unknown") << " | " << sql;
    sqlite3_free(err);
    return false;
  }
  return true;
}

} // namespace

SqliteGraph::SqliteGraph() : db_(nullptr, &sqlite3_close) {}

SqliteGraph::~SqliteGraph()
{
  close();
}

bool SqliteGraph::open(const std::string& dbPath)
{
  std::scoped_lock lock(mutex_);
  if (db_)
    return true;
  sqlite3* raw = nullptr;
  if (sqlite3_open(dbPath.c_str(), &raw) != SQLITE_OK) {
    LOG_ERROR << "SqliteGraph: open failed: "
              << (raw ? sqlite3_errmsg(raw) : "no handle");
    if (raw)
      sqlite3_close(raw);
    return false;
  }
  db_.reset(raw);
  sqlite3_busy_timeout(db_.get(), 5000);
  execUnlocked(db_.get(), "PRAGMA journal_mode = WAL");
  execUnlocked(db_.get(), "PRAGMA synchronous = NORMAL");
  {
    SqliteStmt stmt;
    const bool hasGraph =
        stmt.prepare(db_.get(),
                     "SELECT 1 FROM sqlite_master WHERE type = 'table' AND "
                     "name = 'memory_entity'") &&
        stmt.step() == SQLITE_ROW;
    if (!hasGraph)
      runSchemaFile(db_.get(), "database/schema.sql");
  }
  return true;
}

void SqliteGraph::close()
{
  std::scoped_lock lock(mutex_);
  db_.reset();
}

void SqliteGraph::applySchema()
{
  std::scoped_lock lock(mutex_);
  if (db_)
    runSchemaFile(db_.get(), "database/schema.sql");
}

int64_t SqliteGraph::createEntity(const EntityCreateInput& input)
{
  return repo_.createEntity(db_.get(), input);
}

std::optional<int64_t>
SqliteGraph::resolveEntity(const AliasResolveInput& input)
{
  return repo_.resolveEntity(db_.get(), input);
}

int64_t SqliteGraph::addAlias(const AliasCreateInput& input)
{
  return repo_.addAlias(db_.get(), input);
}

std::vector<AliasInfo> SqliteGraph::aliasesForEntity(int64_t entityId)
{
  return repo_.aliasesForEntity(db_.get(), entityId);
}

int64_t SqliteGraph::upsertFact(const FactUpsertInput& input)
{
  return repo_.upsertFact(db_.get(), input);
}

bool SqliteGraph::closeFact(int64_t factId, int64_t at)
{
  return repo_.closeFact(db_.get(), factId, at);
}

std::vector<RecallHit>
SqliteGraph::factsForEntity(const RecallEntityInput& input)
{
  return repo_.factsForEntity(db_.get(), input);
}

int64_t SqliteGraph::recordEpisode(const EpisodeCreateInput& input)
{
  return repo_.recordEpisode(db_.get(), input);
}

std::vector<int64_t> SqliteGraph::episodesBetween(const std::string& scope,
                                                  int64_t refId, int64_t from,
                                                  int64_t to, int limit)
{
  return repo_.episodesBetween(db_.get(), scope, refId, from, to, limit);
}

int64_t SqliteGraph::createSource(const std::string& channel,
                                  const std::string& turnRef, int64_t at)
{
  return repo_.createSource(db_.get(), channel, turnRef, at);
}

void SqliteGraph::bumpFactHits(const std::vector<int64_t>& factIds)
{
  repo_.bumpFactHits(db_.get(), factIds);
}

int64_t SqliteGraph::recordProcedure(const std::string& name,
                                     const std::string& goal,
                                     const std::string& steps)
{
  return repo_.recordProcedure(db_.get(), name, goal, steps);
}

std::optional<std::string> SqliteGraph::findProcedure(const std::string& goal)
{
  return repo_.findProcedure(db_.get(), goal);
}

void SqliteGraph::migrateLegacy()
{
  std::scoped_lock lock(mutex_);
  const int64_t migrated = repo_.migrateLegacy(db_.get());
  if (migrated > 0)
    LOG_INFO << "SqliteGraph: legacy migration imported " << migrated
             << " memories";
}
