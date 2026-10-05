#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <feature/memory/repositories/memory-graph/memory-graph-repository.hxx>
#include <feature/memory/services/memory/memory-chat.hxx>
#include <feature/memory/services/memory/memory-service.hxx>
#include <feature/memory/services/memory/memory-vec.hxx>
#include <sqlite/sqlite-stmt.hxx>
#include <sqlite/vec-db.hxx>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>
#include <vector>

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#error "ARGUS_TEST_MEMORY_SCHEMA must point at database/schema.sql"
#endif
#ifndef ARGUS_TEST_MEMORY_MODELS_DIR
#error "ARGUS_TEST_MEMORY_MODELS_DIR must point at models/memory"
#endif

namespace
{

constexpr const char* kScratchConfig = "memory-vec-layout-test.toml";
constexpr const char* kScratchDir = "/tmp/argus-memory-vec-layout";
constexpr int64_t kOwner = 7;
constexpr int kDims = 384;

class SilentChat final : public IMemoryChat
{
public:
  [[nodiscard]] bool available() const override { return false; }
  [[nodiscard]] bool busy() const override { return false; }
  [[nodiscard]] std::string chat(const ChatRequest&) const override { return {}; }
};

std::string databaseFile()
{
  return std::string(kScratchDir) + "/layout.db";
}

void writeConfig()
{
  std::remove(kScratchConfig);
  std::ofstream config(kScratchConfig);
  config << "[database]\nfile = \"" << databaseFile() << "\"\n"
         << "[memory]\n"
         << "schema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
         << "create_face_vec = false\n"
         << "embedding_model = \"" << ARGUS_TEST_MEMORY_MODELS_DIR << "/model.onnx\"\n"
         << "embedding_tokenizer = \"" << ARGUS_TEST_MEMORY_MODELS_DIR << "/tokenizer.json\"\n"
         << "embedding_preload = true\n"
         << "[extract]\n"
         << "model_path = \"" << kScratchDir << "/none.gguf\"\n";
}

int64_t scalar(sqlite3* db, const std::string& sql)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, sql.c_str()) || stmt.step() != SQLITE_ROW)
    return -1;
  return stmt.columnInt64(0);
}

std::string jsonVector(int axis)
{
  std::string text = "[";
  for (int index = 0; index < kDims; ++index) {
    if (index > 0)
      text += ",";
    text += index == axis ? "1.0" : "0.0";
  }
  return text + "]";
}

struct LegacyStore
{
  int64_t fact{0};
  int64_t closed{0};
  int64_t episode{0};
};

LegacyStore seedLegacyStore()
{
  VecDb vec;
  vec.setDbFile(databaseFile());
  vec.applySchema(ARGUS_TEST_MEMORY_SCHEMA);
  sqlite3* db = vec.handle();
  REQUIRE(db != nullptr);
  MemoryGraphRepository repo;
  const int64_t entity = repo.createEntity(
      db, {.kind = "pet", .canonical = "perro", .lang = "es", .personId = std::nullopt});
  REQUIRE(entity > 0);
  const auto fact = [&](const std::string& canonical, const std::string& predicate) {
    return repo.upsertFact(db, {.entityId = entity,
                                .predicate = predicate,
                                .value = canonical,
                                .canonical = canonical,
                                .type = "attribute",
                                .priority = 50,
                                .confidence = 0.9F,
                                .lang = "es",
                                .scope = "user",
                                .refId = kOwner,
                                .now = std::time(nullptr),
                                .sourceId = std::nullopt,
                                .supersedes = true});
  };
  LegacyStore store;
  store.closed = fact("mi perro se llama Rex", "mascota");
  store.fact = fact("mi perro se llama Toby", "mascota");
  store.episode = repo.recordEpisode(db, {.kind = "compaction",
                                          .summary = "Hablamos del paseo de Toby por el parque",
                                          .actor = "",
                                          .occurredAt = std::time(nullptr),
                                          .sessionId = {},
                                          .lang = "es",
                                          .scope = "user",
                                          .refId = kOwner,
                                          .salience = 0.7F,
                                          .sourceId = std::nullopt,
                                          .mentionEntityIds = {}});
  REQUIRE(store.closed > 0);
  REQUIRE(store.fact > 0);
  REQUIRE(store.episode > 0);
  REQUIRE(scalar(db, "SELECT count(*) FROM memory_fact WHERE valid_to = 0") == 1);

  const std::string partition = memory_vec::partitionFor("user", kOwner);
  for (const int64_t legacyKey : {store.closed, store.fact, store.episode}) {
    SqliteStmt insert;
    REQUIRE(insert.prepare(db, "INSERT INTO memory_vec (embedding, partition, memory_id, view) "
                               "VALUES (?, ?, ?, 0)"));
    insert.bindText(1, jsonVector(static_cast<int>(legacyKey)));
    insert.bindText(2, partition);
    insert.bindInt64(3, legacyKey);
    REQUIRE(insert.step() == SQLITE_DONE);
  }
  REQUIRE(scalar(db, "SELECT count(*) FROM memory_vec") == 3);
  REQUIRE(scalar(db, "PRAGMA user_version") == 0);
  return store;
}

}

TEST_CASE("a store from before signed vector keys is re-embedded once and loses no memory")
{
  std::filesystem::create_directories(kScratchDir);
  std::filesystem::remove(databaseFile());
  writeConfig();
  ConfigService::load(kScratchConfig);
  const LegacyStore legacy = seedLegacyStore();

  SilentChat chat;
  MemoryService service(VecDb::instance(), chat);
  service.init({});
  REQUIRE(service.isLoaded());
  REQUIRE(service.flushPending(120000));

  sqlite3* db = VecDb::instance().handle();
  REQUIRE(db != nullptr);
  CHECK(scalar(db, "PRAGMA user_version") == memory_vec::kLayout);
  CHECK(scalar(db, "SELECT count(*) FROM memory_fact") == 2);
  CHECK(scalar(db, "SELECT count(*) FROM memory_episode") == 1);
  CHECK(scalar(db, "SELECT count(*) FROM memory_vec WHERE memory_id = " +
                       std::to_string(memory_vec::factKey(legacy.fact))) >= 1);
  CHECK(scalar(db, "SELECT count(*) FROM memory_vec WHERE memory_id = " +
                       std::to_string(memory_vec::episodeKey(legacy.episode))) >= 1);
  CHECK(scalar(db, "SELECT count(*) FROM memory_vec WHERE memory_id = " +
                       std::to_string(memory_vec::factKey(legacy.closed))) == 0);
  CHECK(scalar(db, "SELECT count(*) FROM memory_vec WHERE memory_id NOT IN (" +
                       std::to_string(memory_vec::factKey(legacy.fact)) + ", " +
                       std::to_string(memory_vec::episodeKey(legacy.episode)) + ")") == 0);
  const int64_t rows = scalar(db, "SELECT count(*) FROM memory_vec");
  service.shutdown();

  MemoryService again(VecDb::instance(), chat);
  again.init({});
  REQUIRE(again.flushPending(120000));
  CHECK(scalar(VecDb::instance().handle(), "SELECT count(*) FROM memory_vec") == rows);
  again.shutdown();
  std::remove(kScratchConfig);
}

TEST_CASE("a new store takes the current layout without a rebuild")
{
  std::filesystem::create_directories(kScratchDir);
  const std::string file = std::string(kScratchDir) + "/fresh.db";
  std::filesystem::remove(file);
  VecDb vec;
  vec.setDbFile(file);
  vec.applySchema(ARGUS_TEST_MEMORY_SCHEMA);
  sqlite3* db = vec.handle();
  REQUIRE(db != nullptr);
  MemoryGraphRepository repo;
  CHECK(repo.vecLayout(db) == 0);
  CHECK_FALSE(repo.hasVecRows(db));
  CHECK(repo.setVecLayout(db, memory_vec::kLayout));
  CHECK(repo.vecLayout(db) == memory_vec::kLayout);
}

TEST_CASE("vectors are written only for a memory that still exists")
{
  std::filesystem::create_directories(kScratchDir);
  const std::string file = std::string(kScratchDir) + "/replace.db";
  std::filesystem::remove(file);
  VecDb vec;
  vec.setDbFile(file);
  vec.applySchema(ARGUS_TEST_MEMORY_SCHEMA);
  sqlite3* db = vec.handle();
  REQUIRE(db != nullptr);
  MemoryGraphRepository repo;
  const int64_t entity = repo.createEntity(
      db, {.kind = "pet", .canonical = "gato", .lang = "es", .personId = std::nullopt});
  const int64_t fact = repo.upsertFact(db, {.entityId = entity,
                                            .predicate = "mascota",
                                            .value = "mi gato se llama Miso",
                                            .canonical = "mi gato se llama Miso",
                                            .type = "attribute",
                                            .priority = 50,
                                            .confidence = 0.9F,
                                            .lang = "es",
                                            .scope = "user",
                                            .refId = kOwner,
                                            .now = std::time(nullptr),
                                            .sourceId = std::nullopt,
                                            .supersedes = true});
  REQUIRE(fact > 0);
  const std::string partition = memory_vec::partitionFor("user", kOwner);
  std::vector<float> axis(kDims, 0.0F);
  axis[3] = 1.0F;

  CHECK(repo.replaceVecRows(db, {.key = memory_vec::factKey(fact),
                                 .partition = partition,
                                 .views = {{.view = 0, .vec = axis}, {.view = 1, .vec = axis}}}));
  CHECK(repo.replaceVecRows(db, {.key = memory_vec::factKey(fact),
                                 .partition = partition,
                                 .views = {{.view = 0, .vec = axis}}}));
  CHECK(scalar(db, "SELECT count(*) FROM memory_vec") == 1);
  CHECK(repo.hasVecRows(db));

  CHECK_FALSE(repo.replaceVecRows(db, {.key = memory_vec::episodeKey(fact),
                                       .partition = partition,
                                       .views = {{.view = 0, .vec = axis}}}));
  REQUIRE(repo.forgetFact(db, {.factId = fact, .refId = kOwner}).size() == 1);
  CHECK_FALSE(repo.replaceVecRows(db, {.key = memory_vec::factKey(fact),
                                       .partition = partition,
                                       .views = {{.view = 0, .vec = axis}}}));
  CHECK(scalar(db, "SELECT count(*) FROM memory_vec") == 0);
}
