#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/memory/repositories/memory-graph/memory-graph-repository.hxx>
#include <feature/memory/services/memory/memory-vec.hxx>
#include <sqlite/sqlite-stmt.hxx>
#include <sqlite/vec-db.hxx>

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#error "ARGUS_TEST_MEMORY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr int64_t kOwner = 7;
constexpr int64_t kNeighbour = 9;
constexpr int kDims = 384;

int64_t scalar(sqlite3* db, const std::string& sql)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, sql.c_str()) || stmt.step() != SQLITE_ROW)
    return -1;
  return stmt.columnInt64(0);
}

std::vector<float> unitVector(int axis)
{
  std::vector<float> vec(kDims, 0.0F);
  vec[static_cast<std::size_t>(axis)] = 1.0F;
  return vec;
}

struct Store
{
  Store()
      : path(std::filesystem::temp_directory_path() /
             ("memory-forget-" + std::to_string(::getpid()) + ".db"))
  {
    std::filesystem::remove(path);
    vec.setDbFile(path.string());
    vec.applySchema(ARGUS_TEST_MEMORY_SCHEMA);
  }

  ~Store() { std::filesystem::remove(path); }

  int64_t fact(const std::string& canonical, int64_t refId, int64_t entity)
  {
    return repo.upsertFact(vec.handle(), {.entityId = entity,
                                          .predicate = "mascota",
                                          .value = canonical,
                                          .canonical = canonical,
                                          .type = "attribute",
                                          .priority = 50,
                                          .confidence = 0.9F,
                                          .lang = "es",
                                          .scope = "user",
                                          .refId = refId,
                                          .now = std::time(nullptr),
                                          .sourceId = std::nullopt,
                                          .supersedes = true});
  }

  std::filesystem::path path;
  VecDb vec;
  MemoryGraphRepository repo;
};
}

TEST_CASE("forgetting deletes the fact, its predecessors, its index entry and its vectors")
{
  Store store;
  sqlite3* db = store.vec.handle();
  REQUIRE(db != nullptr);
  const int64_t entity = store.repo.createEntity(
      db, {.kind = "pet", .canonical = "perro", .lang = "es", .personId = std::nullopt});
  REQUIRE(entity > 0);

  const int64_t older = store.fact("mi perro se llama Rex", kOwner, entity);
  const int64_t newer = store.fact("mi perro se llama Toby", kOwner, entity);
  const int64_t theirs = store.fact("mi perro se llama Toby", kNeighbour, entity);
  REQUIRE(older > 0);
  REQUIRE(newer > 0);
  REQUIRE(theirs > 0);

  const std::string partition = memory_vec::partitionFor("user", kOwner);
  const auto toby = unitVector(1);
  const auto episode = unitVector(2);
  store.repo.insertVecRow(db, {.partition = partition,
                               .factId = memory_vec::factKey(newer),
                               .view = 0,
                               .vec = toby});
  store.repo.insertVecRow(db, {.partition = partition,
                               .factId = memory_vec::episodeKey(newer),
                               .view = 0,
                               .vec = episode});
  REQUIRE(scalar(db, "SELECT count(*) FROM memory_vec") == 2);
  const auto nearest = store.repo.vecNeighbours(
      db, {.encoded = memory_vec::encode(toby), .partition = partition, .k = 1});
  REQUIRE(nearest.size() == 1);
  CHECK(nearest.front().factId == memory_vec::factKey(newer));
  CHECK(nearest.front().distance == doctest::Approx(0.0F));

  CHECK(store.repo.forgetFact(db, {.factId = newer, .refId = kNeighbour}).empty());

  const auto forgotten = store.repo.forgetFact(db, {.factId = newer, .refId = kOwner});
  CHECK(forgotten.size() == 2);
  CHECK(scalar(db, "SELECT count(*) FROM memory_fact WHERE id IN (" +
                       std::to_string(older) + ", " + std::to_string(newer) + ")") == 0);
  CHECK(scalar(db, "SELECT count(*) FROM memory_fact WHERE id = " +
                       std::to_string(theirs)) == 1);
  CHECK(scalar(db, "SELECT count(*) FROM memory_fact_fts WHERE memory_fact_fts MATCH "
                   "'rex'") == 0);
  CHECK(scalar(db, "SELECT count(*) FROM memory_fact_fts WHERE memory_fact_fts MATCH "
                   "'toby'") == 1);
  CHECK(scalar(db, "SELECT count(*) FROM memory_vec WHERE memory_id = " +
                       std::to_string(memory_vec::factKey(newer))) == 0);
  CHECK(scalar(db, "SELECT count(*) FROM memory_vec WHERE memory_id = " +
                       std::to_string(memory_vec::episodeKey(newer))) == 1);
  CHECK(scalar(db, "SELECT count(*) FROM memory_edge WHERE kind = 'supersedes'") == 0);
  CHECK(store.repo.forgetFact(db, {.factId = newer, .refId = kOwner}).empty());
}

TEST_CASE("a vector key says whether it names a fact or an episode")
{
  CHECK_FALSE(memory_vec::isEpisodeKey(memory_vec::factKey(42)));
  CHECK(memory_vec::isEpisodeKey(memory_vec::episodeKey(42)));
  CHECK(memory_vec::idOfKey(memory_vec::episodeKey(42)) == 42);
  CHECK(memory_vec::factKey(42) != memory_vec::episodeKey(42));
  CHECK(memory_vec::encode({1.0F, 2.0F}).size() == 2 * sizeof(float));
}
