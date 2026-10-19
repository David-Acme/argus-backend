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
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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
constexpr const char* kScratchConfig = "memory-recall-vec-e2e-test.toml";
constexpr const char* kScratchDir = "/tmp/argus-memory-recall-vec-e2e";
constexpr int64_t kAna = 41;
constexpr int64_t kDavid = 42;
constexpr const char* kAnaFact = "mi perro se llama Rex";
constexpr const char* kDavidFact = "mi perro se llama Toby";

class SilentChat final : public IMemoryChat
{
public:
  [[nodiscard]] bool available() const override { return false; }
  [[nodiscard]] bool busy() const override { return false; }
  [[nodiscard]] std::string chat(const ChatRequest&) const override { return {}; }
};

std::string databaseFile()
{
  return std::string(kScratchDir) + "/e2e.db";
}

bool modelsPresent()
{
  const std::filesystem::path dir(ARGUS_TEST_MEMORY_MODELS_DIR);
  return std::filesystem::exists(dir / "model.onnx") && std::filesystem::exists(dir / "tokenizer.json");
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

void seedFacts()
{
  std::filesystem::create_directories(kScratchDir);
  std::filesystem::remove(databaseFile());
  VecDb vec;
  vec.setDbFile(databaseFile());
  vec.applySchema(ARGUS_TEST_MEMORY_SCHEMA);
  sqlite3* db = vec.handle();
  REQUIRE(db != nullptr);
  MemoryGraphRepository repo;
  const int64_t pet = repo.createEntity(
      db, {.kind = "pet", .canonical = "perro", .lang = "es", .personId = std::nullopt});
  REQUIRE(pet > 0);
  const auto seed = [&](const std::string& canonical, int64_t owner) {
    const int64_t id = repo.upsertFact(db, {.entityId = pet,
                                            .predicate = "mascota",
                                            .value = canonical,
                                            .canonical = canonical,
                                            .type = "attribute",
                                            .priority = 50,
                                            .confidence = 0.9F,
                                            .lang = "es",
                                            .scope = "user",
                                            .refId = owner,
                                            .now = std::time(nullptr),
                                            .sourceId = std::nullopt,
                                            .supersedes = true});
    REQUIRE(id > 0);
  };
  seed(kAnaFact, kAna);
  seed(kDavidFact, kDavid);
}

const std::vector<std::string> kDavidQueries{
    "¿cómo se llama el perro?",
    "¿qué nombre tiene la mascota de la casa?",
    "Rex",
    "dime el nombre del perro de Ana",
};
}

TEST_CASE("the vector tier never returns Ana's fact to David, end to end with the embedding model")
{
  if (!modelsPresent()) {
    std::cerr << "memory-recall-vec-e2e-test: skipped, the embedding model is absent at "
              << ARGUS_TEST_MEMORY_MODELS_DIR << " (run scripts/setup.sh)\n";
    std::exit(77);
  }
  writeConfig();
  ConfigService::load(kScratchConfig);
  seedFacts();

  SilentChat chat;
  MemoryService service(VecDb::instance(), chat);
  service.init({});
  REQUIRE(service.isLoaded());
  REQUIRE(service.flushPending(120000));

  sqlite3* db = VecDb::instance().handle();
  REQUIRE(db != nullptr);
  CHECK(scalar(db, "SELECT count(*) FROM memory_vec WHERE partition = 'user:" + std::to_string(kAna) + "'") >= 1);
  CHECK(scalar(db, "SELECT count(*) FROM memory_vec WHERE partition = 'user:" + std::to_string(kDavid) + "'") >= 1);

  for (const auto& query : kDavidQueries) {
    CAPTURE(query);
    const RecallContext asDavid = service.recall({.userId = kDavid, .text = query, .lang = "es", .personIds = {}});
    CHECK(asDavid.prependText.find("Rex") == std::string::npos);
  }

  const RecallContext asAna = service.recall({.userId = kAna, .text = "Rex", .lang = "es", .personIds = {}});
  CHECK(asAna.prependText.find("Rex") != std::string::npos);

  service.shutdown();
  std::remove(kScratchConfig);
}
