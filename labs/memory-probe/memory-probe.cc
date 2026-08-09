#include <drogon/drogon.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/embedding/embedding-service.hxx>
#include <shared/services/embedding/unigram-tokenizer.hxx>
#include <shared/services/memory/memory-recall.hxx>
#include <shared/services/memory/memory-service.hxx>
#include <shared/services/memory/memory-store.hxx>
#include <shared/services/memory/rule-parser.hxx>
#include <shared/services/memory/simhash.hxx>
#include <shared/services/memory/tool-parser.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace
{

int fails = 0;

void check(bool ok, const std::string& what)
{
  std::cout << (ok ? "[ok] " : "[FAIL] ") << what << "\n";
  if (!ok)
    ++fails;
}

std::string vectorJson(int count, float value)
{
  std::string out = "[";
  for (int i = 0; i < count; ++i) {
    if (i > 0)
      out += ",";
    out += std::to_string(value);
  }
  out += "]";
  return out;
}

void applySchema(sqlite3* db)
{
  std::ifstream file("database/schema.sql");
  if (!file.is_open())
    return;

  std::stringstream buffer;
  buffer << file.rdbuf();

  std::string current;
  std::string line;
  while (std::getline(buffer, line)) {
    auto start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
      continue;
    auto end = line.find_last_not_of(" \t\r\n");
    std::string trimmed = line.substr(start, end - start + 1);
    if (trimmed.rfind("--", 0) == 0)
      continue;
    current += trimmed + "\n";
    if (trimmed.back() == ';') {
      char* err = nullptr;
      if (sqlite3_exec(db, current.c_str(), nullptr, nullptr, &err) !=
          SQLITE_OK) {
        std::cout << "      schema statement failed: "
                  << (err ? err : "unknown") << "\n";
        sqlite3_free(err);
      }
      current.clear();
    }
  }
}

int schemaCheck()
{
  ConfigService::load("config.toml");
  DbService::installExtensions();

  sqlite3* db = nullptr;
  {
    std::scoped_lock lock(VecDb::mutex());
    db = VecDb::handle();
  }
  check(db != nullptr, "VecDb connection opened");
  if (!db)
    return 1;

  applySchema(db);

  sqlite3_stmt* stmt = nullptr;
  const auto tableCount = [&](const char* name) {
    int count = 0;
    if (sqlite3_prepare_v2(
            db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                "AND name = ?",
            -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
    }
    return count;
  };

  check(tableCount("memory_l1") == 1, "memory_l1 table exists");
  check(tableCount("memory_profile") == 1, "memory_profile table exists");
  check(tableCount("memory_fts_words") == 1, "memory_fts_words table exists");
  check(tableCount("memory_fts_grams") == 1, "memory_fts_grams table exists");

  const int64_t id = MemoryStore::save({
      .scope = MemoryScope::User,
      .refId = 1,
      .type = MemoryType::Persona,
      .content = "A Ana no le gusta el pescado",
      .priority = 85,
      .source = MemorySource::Rule,
      .sourceTurnId = std::nullopt,
      .lang = "es",
  });
  check(id > 0, "MemoryStore::save inserted row id=" + std::to_string(id));

  int hits = 0;
  if (sqlite3_prepare_v2(
          db,
          "SELECT COUNT(*) FROM memory_fts_words WHERE "
          "memory_fts_words MATCH 'pescado'",
          -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW)
      hits = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  check(hits == 1, "FTS5 words bm25 match found the row");

  hits = 0;
  if (sqlite3_prepare_v2(
          db,
          "SELECT COUNT(*) FROM memory_fts_grams WHERE "
          "memory_fts_grams MATCH 'pesca'",
          -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW)
      hits = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  check(hits == 1, "FTS5 trigram substring match found the row");

  const auto entries = MemoryStore::list("user", 1, 10);
  check(entries.size() == 1 && entries.front().content ==
                                   "A Ana no le gusta el pescado",
        "MemoryStore::list returns the saved memory");

  constexpr int64_t kVecId = 900001;
  if (sqlite3_prepare_v2(
          db,
          "INSERT INTO memory_vec (rowid, embedding, partition, memory_id) "
          "VALUES (?, ?, ?, ?)",
          -1, &stmt, nullptr) == SQLITE_OK) {
    const std::string enc = vectorJson(384, 1.0F);
    sqlite3_bind_int64(stmt, 1, kVecId);
    sqlite3_bind_text(stmt, 2, enc.data(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, "user:1", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, kVecId);
    check(sqlite3_step(stmt) == SQLITE_DONE, "vec0 insert");
    sqlite3_finalize(stmt);
  } else {
    check(false, "vec0 insert prepare");
  }

  double distance = -1.0;
  if (sqlite3_prepare_v2(
          db,
          "SELECT distance FROM memory_vec WHERE embedding MATCH ? AND "
          "partition = 'user:1' ORDER BY distance LIMIT 1",
          -1, &stmt, nullptr) == SQLITE_OK) {
    const std::string enc = vectorJson(384, 1.0F);
    sqlite3_bind_text(stmt, 1, enc.data(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW)
      distance = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
  }
  check(distance >= 0.0 && distance < 1e-4,
        "vec0 KNN returned the identical vector (distance=" +
            std::to_string(distance) + ")");

  if (sqlite3_prepare_v2(db, "DELETE FROM memory_vec WHERE rowid = ?", -1,
                         &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, kVecId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  MemoryStore::remove(id);
  check(MemoryStore::count() == 0, "cleanup removed test row");

  return fails == 0 ? 0 : 1;
}

int captureTest()
{
  ConfigService::load("config.toml");

  struct Sample
  {
    std::string text;
    std::string lang;
    bool shouldMatch;
  };
  const Sample samples[] = {
      {"recuerda que Ana no come pescado", "es", true},
      {"a partir de ahora siempre saluda al entrar", "es", true},
      {"apunta que hoy llegó el paquete", "es", true},
      {"remember that dad is allergic to peanuts", "en", true},
      {"from now on always lock the door", "en", true},
      {"qué hora es", "es", false},
      {"note that the garden needs watering", "en", true},
  };

  for (const auto& sample : samples) {
    const auto result =
        RuleParser::parse({.text = sample.text, .lang = sample.lang});
    if (sample.shouldMatch) {
      check(result.has_value(),
            "es/en phrase parsed: \"" + sample.text + "\"");
      if (result) {
        std::cout << "      -> type=" << memoryTypeToString(result->type)
                  << " priority=" << result->priority
                  << " content=\"" << result->content << "\"\n";
      }
    } else {
      check(!result.has_value(), "no false match: \"" + sample.text + "\"");
    }
  }

  return fails == 0 ? 0 : 1;
}

int toolParseTest()
{
  ConfigService::load("config.toml");
  const std::string start = ConfigService::getString("memory.save_trigger");
  const std::string end = ConfigService::getString("memory.tool_end_trigger");

  ToolParser parser(start, end);
  std::vector<ToolCall> calls;
  std::string cleaned;
  cleaned += parser.feed("Hola, te cuento: ", calls);
  cleaned += parser.feed(start + "save type=persona priority=85 ", calls);
  cleaned += parser.feed("content=Ana no come pescado", calls);
  cleaned += parser.feed(end + " ¿alguna duda?", calls);
  parser.flush(calls);

  check(calls.size() == 1, "tool block parsed from fragmented stream");
  if (calls.size() == 1) {
    std::cout << "      parsed type=" << memoryTypeToString(calls[0].type)
              << " priority=" << calls[0].priority
              << " content=\"" << calls[0].content << "\"\n";
    check(calls[0].type == MemoryType::Persona &&
              calls[0].priority == 85 &&
              calls[0].content == "Ana no come pescado",
          "tool call fields correct");
  }
  check(cleaned.find(start) == std::string::npos &&
            cleaned.find("save") == std::string::npos,
        "tool block stripped from output text");

  ToolParser parser2(start, end);
  std::vector<ToolCall> calls2;
  const std::string plain = parser2.feed("solo texto normal", calls2);
  check(calls2.empty() && plain == "solo texto normal",
        "plain text passes through untouched");

  ToolParser parser3(start, end);
  std::vector<ToolCall> calls3;
  parser3.feed(start + "save type=bogus content=", calls3);
  parser3.flush(calls3);
  check(calls3.empty(), "malformed tool block dropped safely");

  return fails == 0 ? 0 : 1;
}

float cosine(const std::vector<float>& a, const std::vector<float>& b)
{
  float dot = 0.0F;
  for (size_t i = 0; i < a.size(); ++i)
    dot += a[i] * b[i];
  return dot;
}

int embedCheck()
{
  ConfigService::load("config.toml");
  MemoryService::init();
  if (!EmbeddingService::isLoaded()) {
    check(false, "embedding model loaded (run scripts/setup.sh)");
    return 1;
  }

  const auto gato = EmbeddingService::embed("gato", "query:");
  const auto gatito = EmbeddingService::embed("gatito", "query:");
  const auto computadora = EmbeddingService::embed("computadora", "query:");
  const auto elGato = EmbeddingService::embed("el gato", "query:");
  const auto theCat = EmbeddingService::embed("the cat", "query:");
  const auto pescado = EmbeddingService::embed("pescado", "query:");

  check(gato && gatito && computadora && elGato && theCat && pescado,
        "embeddings produced");

  const float cGatito = cosine(*gato, *gatito);
  const float cComp = cosine(*gato, *computadora);
  const float cElGato = cosine(*gato, *elGato);
  const float cTheCat = cosine(*gato, *theCat);
  const float cPescado = cosine(*gato, *pescado);
  std::cout << "      gato-gatito=" << cGatito << " gato-computadora=" << cComp
            << " gato-elgato=" << cElGato << " gato-thecat=" << cTheCat
            << " gato-pescado=" << cPescado << "\n";
  check(cGatito > cComp + 0.05F, "gato/gatito ranks above unrelated words");
  check(cElGato > cPescado + 0.05F, "gato/el gato ranks above pescado");
  check(cTheCat > cComp + 0.02F, "cross-lingual es/en ranks above unrelated");
  check(cGatito > 0.5F, "gato/gatito meaningfully similar");

  MemoryService::shutdown();
  return fails == 0 ? 0 : 1;
}

int recallBench()
{
  ConfigService::load("config.toml");
  DbService::installExtensions();

  sqlite3* db = nullptr;
  {
    std::scoped_lock lock(VecDb::mutex());
    db = VecDb::handle();
  }
  if (!db)
    return 1;
  applySchema(db);

  MemoryService::init();
  if (!EmbeddingService::isLoaded()) {
    check(false, "embedding model loaded (run scripts/setup.sh)");
    return 1;
  }

  const char* seeds[] = {
      "A Ana no le gusta el pescado",
      "Al abuelo le encanta el café con leche",
      "María trabaja de lunes a viernes en el hospital",
      "El perro se llama Toby y duerme en el sofá",
      "La puerta de la cocina se atasca en invierno",
      "El código del wifi es argus2026",
      "Los domingos se riega el jardín por la mañana",
      "A Luis le da miedo la oscuridad",
      "La abuela es alérgica a los frutos secos",
      "El termostato se pone a 21 grados en invierno",
      "Los niños salen del colegio a las 5",
      "El vecino del 3 tiene un gato negro",
      "La cámara de la entrada avisa cuando llega el cartero",
      "La contraseña de la app se cambia cada 3 meses",
      "A Carlos le gusta el fútbol los sábados",
      "El horno hace un pitido cuando termina",
      "La abuela viene a cenar los viernes",
      "El agua del grifo de la cocina sabe a cloro",
      "A Marta le gusta leer antes de dormir",
      "El garaje se cierra solo a las 11 de la noche",
  };

  std::vector<int64_t> ids;
  for (const char* seed : seeds) {
    const int64_t id = MemoryService::captureExplicit(
        {.userId = 7, .lang = "es", .text = std::string("recuerda que ") +
                                                 seed});
    if (id > 0)
      ids.push_back(id);
  }
  check(ids.size() == 20, "seeded 20 memories");

  const char* queries[] = {
      "¿qué no le gusta comer a Ana?",
      "¿cuándo sale la abuela?",
      "¿cómo se llama el perro?",
      "¿cuál es el wifi?",
      "¿qué hace Marta por la noche?",
      "¿a qué hora cierra el garaje?",
      "¿quién trabaja en el hospital?",
      "¿cuándo se riega el jardín?",
      "¿qué le pasa al horno?",
      "¿quién tiene miedo a la oscuridad?",
  };

  std::vector<double> latencies;
  size_t totalChars = 0;
  size_t totalHits = 0;
  for (int run = 0; run < 30; ++run) {
    const std::string& query = queries[run % 10];
    const auto t0 = std::chrono::steady_clock::now();
    const auto ctx =
        MemoryService::recall({.userId = 7, .text = query, .lang = "es", .personIds = {}});
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    latencies.push_back(ms);
    totalChars += ctx.prependText.size();
    totalHits += ctx.usedIds.size();
  }
  std::sort(latencies.begin(), latencies.end());
  const double p50 = latencies[latencies.size() / 2];
  const double p95 = latencies[static_cast<size_t>(latencies.size() * 0.95)];
  std::cout << "[ok] recall bench: p50=" << p50 << "ms p95=" << p95
            << "ms avg_injected=" << (totalChars / 30 / 4)
            << " tokens, avg_hits=" << (totalHits / 30) << "\n";

  for (int64_t id : ids)
    MemoryStore::remove(id);
  check(MemoryStore::count() == 0, "bench cleanup");
  MemoryService::shutdown();
  return fails == 0 ? 0 : 1;
}

int tokenDebug(const std::string& text)
{
  ConfigService::load("config.toml");
  UnigramTokenizer tok;
  if (!tok.load(ConfigService::getString("memory.embedding_tokenizer"))) {
    std::cout << "tokenizer load failed\n";
    return 1;
  }
  const auto ids = tok.encode(text, 512);
  std::cout << "[";
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i > 0)
      std::cout << ", ";
    std::cout << ids[i];
  }
  std::cout << "]\n";
  return 0;
}

void usage()
{
  std::cout << "argus-memory-probe --schema-check | --capture-test | "
               "--tool-parse-test | --embed-check | --recall-bench | "
               "--tokens <text>\n";
}

} // namespace

int main(int argc, char** argv)
{
  if (argc < 2) {
    usage();
    return 1;
  }

  const std::string mode = argv[1];
  if (mode == "--schema-check")
    return schemaCheck();
  if (mode == "--capture-test")
    return captureTest();
  if (mode == "--tool-parse-test")
    return toolParseTest();
  if (mode == "--embed-check")
    return embedCheck();
  if (mode == "--recall-bench")
    return recallBench();
  if (mode == "--tokens" && argc >= 3)
    return tokenDebug(argv[2]);

  usage();
  return 1;
}
