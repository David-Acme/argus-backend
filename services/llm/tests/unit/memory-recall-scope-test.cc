#include <doctest/doctest.h>

#include <feature/memory/repositories/memory-graph/memory-graph-repository.hxx>
#include <feature/memory/services/memory/graph-recall.hxx>
#include <feature/memory/services/memory/memory-vec.hxx>
#include <sqlite/sqlite-stmt.hxx>
#include <sqlite/vec-db.hxx>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#error "ARGUS_TEST_MEMORY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr int64_t kAna = 11;
constexpr int64_t kDavid = 12;
constexpr int kDims = 384;

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
             ("memory-recall-scope-" + std::to_string(::getpid()) + ".db"))
  {
    std::filesystem::remove(path);
    vec.setDbFile(path.string());
    vec.applySchema(ARGUS_TEST_MEMORY_SCHEMA);
  }

  ~Store() { std::filesystem::remove(path); }

  int64_t fact(int64_t entity, const std::string& predicate, const std::string& canonical,
               const std::string& scope, int64_t refId)
  {
    return repo.upsertFact(vec.handle(), {.entityId = entity,
                                          .predicate = predicate,
                                          .value = canonical,
                                          .canonical = canonical,
                                          .type = "attribute",
                                          .priority = 50,
                                          .confidence = 0.9F,
                                          .lang = "es",
                                          .scope = scope,
                                          .refId = refId,
                                          .now = std::time(nullptr),
                                          .sourceId = std::nullopt,
                                          .supersedes = true});
  }

  int64_t episode(const std::string& summary, const std::string& scope, int64_t refId)
  {
    return repo.recordEpisode(vec.handle(), {.kind = "event",
                                             .summary = summary,
                                             .actor = "",
                                             .occurredAt = std::time(nullptr),
                                             .sessionId = "",
                                             .lang = "es",
                                             .scope = scope,
                                             .refId = refId,
                                             .salience = 0.8F,
                                             .sourceId = std::nullopt,
                                             .mentionEntityIds = {}});
  }

  std::filesystem::path path;
  VecDb vec;
  MemoryGraphRepository repo;
};

std::vector<RecallAudience> davidsAudiences()
{
  return recallAudiences(kDavid);
}

bool contains(const std::vector<int64_t>& ids, int64_t id)
{
  return std::ranges::find(ids, id) != ids.end();
}
}

TEST_CASE("a listener's audiences are its own personal facts and the household, nobody else's")
{
  const auto audiences = davidsAudiences();
  REQUIRE(audiences.size() == 2);
  CHECK(audiences[0].scope == MemoryScope::Personal);
  CHECK(audiences[0].refId == kDavid);
  CHECK(audiences[1].scope == MemoryScope::Household);
  CHECK(audiences[1].refId == 0);
  for (const auto& audience : audiences)
    CHECK(audience.refId != kAna);
}

TEST_CASE("the entity tier never returns another person's personal fact")
{
  Store store;
  sqlite3* db = store.vec.handle();
  REQUIRE(db != nullptr);
  const int64_t pet = store.repo.createEntity(
      db, {.kind = "pet", .canonical = "perro", .lang = "es", .personId = std::nullopt});
  REQUIRE(pet > 0);
  const int64_t anaFact = store.fact(pet, "mascota", "el perro de Ana se llama Rex", "user", kAna);
  const int64_t davidFact = store.fact(pet, "mascota", "mi perro se llama Toby", "user", kDavid);
  const int64_t house = store.fact(pet, "mascota_casa", "el perro de la casa se llama Max", "household", 0);
  REQUIRE(anaFact > 0);
  REQUIRE(davidFact > 0);
  REQUIRE(house > 0);

  std::vector<int64_t> seen;
  for (const auto& audience : davidsAudiences()) {
    for (const auto& hit : store.repo.factsForEntity(
             db, {.entityId = pet,
                  .scope = memoryScopeToString(audience.scope),
                  .refId = audience.refId,
                  .maxHops = 1,
                  .limit = 10}))
      seen.push_back(hit.factId);
  }
  CHECK(contains(seen, davidFact));
  CHECK(contains(seen, house));
  CHECK_FALSE(contains(seen, anaFact));
}

TEST_CASE("the lexical tier never returns another person's personal fact")
{
  Store store;
  sqlite3* db = store.vec.handle();
  REQUIRE(db != nullptr);
  const int64_t pet = store.repo.createEntity(
      db, {.kind = "pet", .canonical = "perro", .lang = "es", .personId = std::nullopt});
  REQUIRE(pet > 0);
  const int64_t anaFact = store.fact(pet, "mascota", "el perro de Ana se llama Rex", "user", kAna);
  const int64_t house = store.fact(pet, "mascota_casa", "el perro de la casa se llama Max", "household", 0);
  REQUIRE(anaFact > 0);
  REQUIRE(house > 0);

  std::vector<int64_t> asDavid;
  for (const auto& audience : davidsAudiences()) {
    for (const auto& hit : store.repo.ftsFacts(
             db, {.match = "\"Rex\"",
                  .scope = memoryScopeToString(audience.scope),
                  .refId = audience.refId,
                  .limit = 10}))
      asDavid.push_back(hit.factId);
  }
  CHECK_FALSE(contains(asDavid, anaFact));

  const auto asAna = store.repo.ftsFacts(
      db, {.match = "\"Rex\"", .scope = "user", .refId = kAna, .limit = 10});
  REQUIRE(!asAna.empty());
  CHECK(asAna.front().factId == anaFact);

  std::vector<int64_t> household;
  for (const auto& audience : davidsAudiences()) {
    for (const auto& hit : store.repo.ftsFacts(
             db, {.match = "\"Max\"",
                  .scope = memoryScopeToString(audience.scope),
                  .refId = audience.refId,
                  .limit = 10}))
      household.push_back(hit.factId);
  }
  CHECK(contains(household, house));
}

TEST_CASE("the vector tier partitions by owner and a foreign fact id is refused by factById")
{
  Store store;
  sqlite3* db = store.vec.handle();
  REQUIRE(db != nullptr);
  const int64_t pet = store.repo.createEntity(
      db, {.kind = "pet", .canonical = "perro", .lang = "es", .personId = std::nullopt});
  REQUIRE(pet > 0);
  const int64_t anaFact = store.fact(pet, "mascota", "el perro de Ana se llama Rex", "user", kAna);
  const int64_t davidFact = store.fact(pet, "mascota", "mi perro se llama Toby", "user", kDavid);
  const int64_t house = store.fact(pet, "mascota_casa", "el perro de la casa se llama Max", "household", 0);
  REQUIRE(anaFact > 0);
  REQUIRE(davidFact > 0);
  REQUIRE(house > 0);
  store.repo.insertVecRow(db, {.partition = memory_vec::partitionFor("user", kAna),
                               .factId = memory_vec::factKey(anaFact),
                               .view = 0,
                               .vec = unitVector(1)});
  store.repo.insertVecRow(db, {.partition = memory_vec::partitionFor("user", kDavid),
                               .factId = memory_vec::factKey(davidFact),
                               .view = 0,
                               .vec = unitVector(2)});
  store.repo.insertVecRow(db, {.partition = memory_vec::partitionFor("household", 0),
                               .factId = memory_vec::factKey(house),
                               .view = 0,
                               .vec = unitVector(3)});

  const std::string encoded = memory_vec::encode(unitVector(1));
  std::vector<int64_t> reached;
  for (const auto& audience : davidsAudiences()) {
    const auto neighbours = store.repo.vecNeighbours(
        db, {.encoded = encoded,
             .partition = memory_vec::partitionFor(memoryScopeToString(audience.scope), audience.refId),
             .k = 5});
    for (const auto& neighbour : neighbours) {
      reached.push_back(neighbour.factId);
      const auto fact = store.repo.factById(
          db, {.factId = neighbour.factId,
               .scope = memoryScopeToString(audience.scope),
               .refId = audience.refId});
      if (fact)
        CHECK(fact->factId != anaFact);
    }
  }
  CHECK_FALSE(contains(reached, memory_vec::factKey(anaFact)));
  CHECK(contains(reached, memory_vec::factKey(davidFact)));

  for (const auto& audience : davidsAudiences()) {
    CHECK_FALSE(store.repo.factById(db, {.factId = anaFact,
                                         .scope = memoryScopeToString(audience.scope),
                                         .refId = audience.refId})
                    .has_value());
  }
}

TEST_CASE("the episode tiers never return another person's episode, by search or by id")
{
  Store store;
  sqlite3* db = store.vec.handle();
  REQUIRE(db != nullptr);
  const int64_t anaEpisode = store.episode("Ana cumple años el sábado", "user", kAna);
  const int64_t house = store.episode("la casa cumple años el sábado", "household", 0);
  REQUIRE(anaEpisode > 0);
  REQUIRE(house > 0);

  std::vector<int64_t> found;
  for (const auto& audience : davidsAudiences()) {
    for (const auto& hit : store.repo.ftsEpisodes(
             db, {.match = "\"cumple\"",
                  .scope = memoryScopeToString(audience.scope),
                  .refId = audience.refId,
                  .limit = 5}))
      found.push_back(hit.episodeId);
    CHECK_FALSE(store.repo.episodeById(db, {.episodeId = anaEpisode,
                                            .scope = memoryScopeToString(audience.scope),
                                            .refId = audience.refId})
                    .has_value());
  }
  CHECK_FALSE(contains(found, anaEpisode));
  CHECK(contains(found, house));
}
