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

struct FactCloseInput
{
  int64_t factId{0};
  int64_t at{0};
};

struct FactSearchInput
{
  std::string match;
  std::string scope;
  int64_t refId{0};
  int limit{0};
};

struct ContentQueryInput
{
  int64_t id;
  std::string& scope;
  int64_t& refId;
};

struct HitsBumpInput
{
  const std::vector<int64_t>& ids;
  int64_t at;
};

struct FactBumpInput
{
  int64_t factId{0};
  int64_t at{0};
};

struct ProfileFactsInput
{
  int64_t refId{0};
  int limit{0};
};

struct FtsIndexInput
{
  std::string table;
  std::string column;
  int64_t rowid{0};
  std::string text;
};

struct EdgeInsertInput
{
  std::string kind;
  int64_t src{0};
  int64_t dst{0};
  std::string predicate{};
  int64_t since{0};
  int64_t until{0};
  int ord{0};
};

struct VecRowInsertInput
{
  std::string partition;
  int64_t factId;
  int view;
  const std::vector<float>& vec;
};

struct VecDedupInput
{
  std::string encoded;
  std::string partition;
  int64_t factId{0};
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
  bool closeFact(sqlite3* db, const FactCloseInput& input);
  std::vector<RecallHit> factsForEntity(sqlite3* db,
                                        const RecallEntityInput& input);
  std::vector<VecNeighbour> vecNeighbours(sqlite3* db,
                                          const VecNeighbourInput& input);
  std::optional<RecallHit> factById(sqlite3* db,
                                    const FactByIdInput& input);
  std::vector<RecallHit> ftsFacts(sqlite3* db, const FactSearchInput& input);
  int64_t recordEpisode(sqlite3* db, const EpisodeCreateInput& input);
  std::vector<int64_t> episodesBetween(sqlite3* db,
                                       const EpisodesBetweenInput& input);
  std::optional<EpisodeHit> episodeById(sqlite3* db,
                                        const EpisodeByIdInput& input);
  std::vector<EpisodeHit> ftsEpisodes(sqlite3* db,
                                      const FactSearchInput& input);
  std::optional<std::string> episodeContent(sqlite3* db,
                                            const ContentQueryInput& input);
  void bumpEpisodeHits(sqlite3* db, const HitsBumpInput& input);
  void bumpFactImportance(sqlite3* db, const FactBumpInput& input);
  std::vector<ProfileFactRow> topProfileFacts(sqlite3* db,
                                              const ProfileFactsInput& input);
  int64_t createSource(sqlite3* db, const SourceCreateInput& input);
  void bumpFactHits(sqlite3* db, const std::vector<int64_t>& factIds);
  int64_t recordProcedure(sqlite3* db, const ProcedureRecordInput& input);
  std::optional<std::string> findProcedure(sqlite3* db,
                                           const std::string& goal);

  void ftsInsert(sqlite3* db, const FtsIndexInput& input);
  void insertEdge(sqlite3* db, const EdgeInsertInput& input);

  std::optional<std::string> factContent(sqlite3* db,
                                         const ContentQueryInput& input);
  std::vector<int64_t> openFactIds(sqlite3* db);
  void insertVecRow(sqlite3* db, const VecRowInsertInput& input);
  float vecDedupSim(sqlite3* db, const VecDedupInput& input);
  void deleteVecRows(sqlite3* db, int64_t factId);

  std::vector<GazetteerRow> gazetteerAliases(sqlite3* db);
  std::vector<CatalogRow> catalogPersons(sqlite3* db);
  std::vector<CatalogRow> catalogCameras(sqlite3* db);
  std::vector<CatalogRow> catalogZones(sqlite3* db);
  std::vector<CatalogRow> catalogStreams(sqlite3* db);

  int64_t migrateLegacy(sqlite3* db);
};
