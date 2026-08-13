#pragma once

#include <cstdint>
#include <optional>
#include <shared/repositories/memory-graph/memory-graph-query.hxx>
#include <shared/services/memory/semantic-graph.hxx>
#include <string>
#include <vector>

struct sqlite3;

struct GazetteerRow
{
  std::string norm;
  std::string surface;
  std::string personFrame;
  int64_t entityId;
  std::string kind;
};

struct VecNeighbour
{
  int64_t factId;
  float distance;
};

struct CatalogRow
{
  std::string surface;
  std::string kind;
  int64_t catalogId;
};

class MemoryGraphRepository
{
public:
  int64_t createEntity(sqlite3* db, const EntityCreateInput& input);
  std::optional<int64_t> resolveEntity(sqlite3* db,
                                       const AliasResolveInput& input);
  int64_t addAlias(sqlite3* db, const AliasCreateInput& input);
  std::vector<AliasInfo> aliasesForEntity(sqlite3* db, int64_t entityId);
  int64_t upsertFact(sqlite3* db, const FactUpsertInput& input);
  bool closeFact(sqlite3* db, int64_t factId, int64_t at);
  std::vector<RecallHit> factsForEntity(sqlite3* db,
                                        const RecallEntityInput& input);
  std::vector<VecNeighbour> vecNeighbours(sqlite3* db,
                                          const VecNeighbourInput& input);
  std::optional<RecallHit> factById(sqlite3* db,
                                    const FactByIdInput& input);
  std::vector<RecallHit> ftsFacts(sqlite3* db, const std::string& match,
                                  const std::string& scope, int64_t refId,
                                  int limit);
  int64_t recordEpisode(sqlite3* db, const EpisodeCreateInput& input);
  std::vector<int64_t> episodesBetween(sqlite3* db, const std::string& scope,
                                       int64_t refId, int64_t from, int64_t to,
                                       int limit);
  std::optional<EpisodeHit> episodeById(sqlite3* db,
                                        const EpisodeByIdInput& input);
  std::vector<EpisodeHit> ftsEpisodes(sqlite3* db, const std::string& match,
                                      const std::string& scope, int64_t refId,
                                      int limit);
  std::optional<std::string> episodeContent(sqlite3* db, int64_t episodeId,
                                            std::string& scope,
                                            int64_t& refId);
  void bumpEpisodeHits(sqlite3* db, const std::vector<int64_t>& ids,
                       int64_t at);
  void bumpFactImportance(sqlite3* db, int64_t factId, int64_t at);
  std::vector<ProfileFactRow> topProfileFacts(sqlite3* db, int64_t refId,
                                              int limit);
  int64_t createSource(sqlite3* db, const std::string& channel,
                       const std::string& turnRef, int64_t at);
  void bumpFactHits(sqlite3* db, const std::vector<int64_t>& factIds);
  int64_t recordProcedure(sqlite3* db, const std::string& name,
                          const std::string& goal, const std::string& steps);
  std::optional<std::string> findProcedure(sqlite3* db,
                                           const std::string& goal);

  void ftsInsert(sqlite3* db, const char* table, const char* column,
                 int64_t rowid, const std::string& text);
  void insertEdge(sqlite3* db, const std::string& kind, int64_t src,
                  int64_t dst, const std::string& predicate, int64_t since,
                  int64_t until, int ord);

  std::optional<std::string> factContent(sqlite3* db, int64_t factId,
                                         std::string& scope, int64_t& refId);
  std::vector<int64_t> openFactIds(sqlite3* db);
  void insertVecRow(sqlite3* db, const std::string& partition, int64_t factId,
                    int view, const std::vector<float>& vec);
  float vecDedupSim(sqlite3* db, const std::string& encoded,
                    const std::string& partition, int64_t factId);
  void deleteVecRows(sqlite3* db, int64_t factId);

  std::vector<GazetteerRow> gazetteerAliases(sqlite3* db);
  std::vector<CatalogRow> catalogPersons(sqlite3* db);
  std::vector<CatalogRow> catalogCameras(sqlite3* db);
  std::vector<CatalogRow> catalogZones(sqlite3* db);
  std::vector<CatalogRow> catalogStreams(sqlite3* db);

  int64_t migrateLegacy(sqlite3* db);
};
