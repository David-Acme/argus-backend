#pragma once

#include "semantic-graph.hxx"

#include <memory>
#include <mutex>
#include <shared/repositories/memory-graph/memory-graph-repository.hxx>
#include <string>

struct sqlite3;

class SqliteGraph final : public SemanticGraph
{
public:
  SqliteGraph();
  ~SqliteGraph() override;

  SqliteGraph(const SqliteGraph&) = delete;
  SqliteGraph& operator=(const SqliteGraph&) = delete;

  bool open(const std::string& dbPath);
  void close();
  bool isOpen() const { return db_ != nullptr; }

  std::mutex& mutex() { return mutex_; }
  sqlite3* handle() { return db_.get(); }
  void applySchema();

  void migrateLegacy();
  void bumpFactHits(const std::vector<int64_t>& factIds);
  int64_t recordProcedure(const std::string& name, const std::string& goal,
                          const std::string& steps);
  std::optional<std::string> findProcedure(const std::string& goal);

  int64_t createEntity(const EntityCreateInput& input) override;
  std::optional<int64_t> resolveEntity(const AliasResolveInput& input) override;
  int64_t addAlias(const AliasCreateInput& input) override;
  std::vector<AliasInfo> aliasesForEntity(int64_t entityId) override;
  int64_t upsertFact(const FactUpsertInput& input) override;
  bool closeFact(int64_t factId, int64_t at) override;
  std::vector<RecallHit>
  factsForEntity(const RecallEntityInput& input) override;
  int64_t recordEpisode(const EpisodeCreateInput& input) override;
  std::vector<int64_t> episodesBetween(const std::string& scope, int64_t refId,
                                       int64_t from, int64_t to,
                                       int limit) override;
  int64_t createSource(const std::string& channel, const std::string& turnRef,
                       int64_t at) override;

private:
  std::unique_ptr<sqlite3, int (*)(sqlite3*)> db_;
  std::mutex mutex_;
  MemoryGraphRepository repo_;
};
