#include <algorithm>
#include <chrono>
#include <cmath>
#include <drogon/drogon.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/embedding/embedding-service.hxx>
#include <shared/services/embedding/unigram-tokenizer.hxx>
#include <shared/services/extract/tiered-extractor.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/memory/entity-resolver.hxx>
#include <shared/services/memory/graph-recall.hxx>
#include <shared/services/memory/memory-formation.hxx>
#include <shared/services/memory/memory-service.hxx>
#include <shared/services/memory/phrase-catalog.hxx>
#include <shared/services/memory/rule-parser.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/services/memory/tool-parser.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <shared/services/tools/tool-executor.hxx>
#include <shared/services/tools/tool-registry.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <vector>

namespace
{

int fails = 0;
VecDb gVecDb;
LlmService gLlm;
MemoryService gMemory{gVecDb, gLlm};
EmbeddingService gEmbedding;

void clearUserRows(int64_t userId);

constexpr int64_t kProbeUser = 990001;
constexpr int64_t kFixtureUser = 990007;

void check(bool ok, const std::string& what)
{
  std::cout << (ok ? "[ok] " : "[FAIL] ") << what << "\n";
  if (!ok)
    ++fails;
}

struct ParserFixture
{
  SqliteGraph graph;
  PhraseCatalog catalog{graph};
  RuleParser parser{catalog};

  explicit ParserFixture(const std::string& dbPath)
  {
    graph.open(dbPath);
    graph.applySchema();
    catalog.build();
  }
};

int64_t newestFactId(int64_t userId)
{
  std::scoped_lock lock(gVecDb.mutex());
  sqlite3* db = gVecDb.handle();
  SqliteStmt stmt;
  if (!db || !stmt.prepare(db, "SELECT id FROM memory_fact WHERE scope = "
                               "'user' AND ref_id = ? AND valid_to = 0 "
                               "ORDER BY id DESC LIMIT 1"))
    return 0;
  stmt.bindInt64(1, userId);
  return stmt.step() == SQLITE_ROW ? stmt.columnInt64(0) : 0;
}

int64_t captureAndSettle(int64_t userId, const std::string& lang,
                         const std::string& text)
{
  const auto captured =
      gMemory.captureExplicit({.userId = userId, .lang = lang, .text = text});
  if (captured.outcome == CaptureOutcome::Stored)
    return captured.factId;
  if (captured.outcome != CaptureOutcome::Deferred)
    return 0;
  gMemory.flushPending();
  return newestFactId(userId);
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

int captureQuery(const std::string& text)
{
  ConfigService::load("config.toml");
  ParserFixture fixture(ConfigService::getString("database.file"));
  const auto phrase = fixture.parser.parse({.text = text, .lang = "es"});
  const auto stmt = fixture.parser.parseStatement({.text = text, .lang = "es"});
  std::cout << "text: \"" << text << "\"\n";
  std::cout << "  parse():          "
            << (phrase ? "MATCH content=\"" + phrase->content + "\""
                       : "no match")
            << "\n";
  std::cout << "  parseStatement(): "
            << (stmt ? "MATCH content=\"" + stmt->content + "\"" : "no match")
            << "\n";
  return 0;
}

int schemaCheck()
{
  ConfigService::load("config.toml");
  DbService::installExtensions();

  sqlite3* db = nullptr;
  {
    std::scoped_lock lock(gVecDb.mutex());
    db = gVecDb.handle();
  }
  check(db != nullptr, "VecDb connection opened");
  if (!db)
    return 1;

  gVecDb.applySchema();

  SqliteStmt stmt;
  const auto tableCount = [&](const char* name) {
    int count = 0;
    if (stmt.prepare(db,
                     "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                     "AND name = ?")) {
      stmt.bindText(1, name);
      if (stmt.step() == SQLITE_ROW)
        count = stmt.columnInt(0);
      stmt.finalize();
    }
    return count;
  };

  check(tableCount("memory_entity") == 1, "memory_entity table exists");
  check(tableCount("memory_alias") == 1, "memory_alias table exists");
  check(tableCount("memory_fact") == 1, "memory_fact table exists");
  check(tableCount("memory_edge") == 1, "memory_edge table exists");
  check(tableCount("memory_episode") == 1, "memory_episode table exists");
  check(tableCount("memory_source") == 1, "memory_source table exists");
  check(tableCount("memory_procedure") == 1, "memory_procedure table exists");
  check(tableCount("memory_fact_fts") == 1, "memory_fact_fts exists");
  check(tableCount("job") == 1, "job table exists");

  gMemory.init();
  const int64_t id =
      captureAndSettle(kProbeUser, "es", "recuerda que no me gusta el pescado");
  check(id > 0, "captureExplicit inserted fact id=" + std::to_string(id));
  gMemory.flushPending();
  const auto ctx = gMemory.recall({.userId = kProbeUser,
                                   .text = "a quien no le gusta el pescado",
                                   .lang = "es",
                                   .personIds = {}});
  check(std::find(ctx.usedIds.begin(), ctx.usedIds.end(), id) !=
            ctx.usedIds.end(),
        "captured fact is recallable");

  const auto deferStart = std::chrono::steady_clock::now();
  const auto deferred = gMemory.captureExplicit(
      {.userId = kProbeUser,
       .lang = "es",
       .text = "recuerda que la red domestica tiene la clave argus2026"});
  const auto deferMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - deferStart)
                           .count();
  check(deferred.outcome == CaptureOutcome::Deferred,
        "model-tier utterance is deferred, not extracted inline");
  check(deferMs < 50, "turn path stays under 50 ms (took " +
                          std::to_string(deferMs) + " ms)");
  gMemory.flushPending();
  check(newestFactId(kProbeUser) > id,
        "deferred utterance lands through the worker");

  clearUserRows(kProbeUser);
  gMemory.shutdown();

  constexpr int64_t kVecId = 900001;
  if (stmt.prepare(
          db, "INSERT INTO memory_vec (rowid, embedding, partition, memory_id, "
              "view) VALUES (?, ?, ?, ?, ?)")) {
    const std::string enc = vectorJson(gVecDb.embeddingDims(), 1.0F);
    stmt.bindInt64(1, kVecId);
    stmt.bindText(2, enc);
    stmt.bindText(3, "user:" + std::to_string(kProbeUser));
    stmt.bindInt64(4, kVecId);
    stmt.bindInt(5, 0);
    check(stmt.step() == SQLITE_DONE, "vec0 insert");
    stmt.finalize();
  }
  else {
    check(false, "vec0 insert prepare");
  }

  double distance = -1.0;
  if (stmt.prepare(
          db, "SELECT distance FROM memory_vec WHERE embedding MATCH ? AND "
              "partition = ? ORDER BY distance LIMIT 1")) {
    const std::string enc = vectorJson(gVecDb.embeddingDims(), 1.0F);
    stmt.bindText(1, enc);
    stmt.bindText(2, "user:" + std::to_string(kProbeUser));
    if (stmt.step() == SQLITE_ROW)
      distance = stmt.columnDouble(0);
    stmt.finalize();
  }
  check(std::fabs(distance) < 1e-4,
        "vec0 KNN returned the identical vector (distance=" +
            std::to_string(distance) + ")");

  if (stmt.prepare(db, "DELETE FROM memory_vec WHERE rowid = ?")) {
    stmt.bindInt64(1, kVecId);
    stmt.step();
    stmt.finalize();
  }

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
      {"mi hermana viene los domingos a comer", "es", true},
      {"la alarma de la puerta se activa a las 10", "es", true},
      {"my sister comes on sundays for lunch", "en", true},
      {"the wifi code is argus2026", "en", true},
      {"¿cuándo viene mi hermana?", "es", false},
      {"el clima está agradable hoy", "es", false},
      {"hola argus, recuérdame que mi hermana viene los domingos", "es", true},
      {"oye argus, recuerda que la alarma suena a las 10", "es", true},
      {"hello argus, remind me that my brother comes on friday", "en", true},
      {"hey argus, my sister is allergic to shrimp", "en", true},
  };

  ParserFixture fixture(ConfigService::getString("database.file"));
  const auto greeting = fixture.parser.parse(
      {.text = "hola argus, recuérdame que mi hermana viene los domingos",
       .lang = "es"});
  check(greeting && greeting->content == "mi hermana viene los domingos",
        "greeting stripped before phrase capture");

  for (const auto& sample : samples) {
    auto result =
        fixture.parser.parse({.text = sample.text, .lang = sample.lang});
    const bool statement = !result.has_value();
    if (!result)
      result = fixture.parser.parseStatement(
          {.text = sample.text, .lang = sample.lang});
    if (sample.shouldMatch) {
      check(result.has_value(), "es/en phrase parsed: \"" + sample.text + "\"");
      if (result) {
        std::cout << "      -> type=" << memoryTypeToString(result->type)
                  << " priority=" << result->priority
                  << (statement ? " [statement]" : "") << " content=\""
                  << result->content << "\"\n";
      }
    }
    else {
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
              << " priority=" << calls[0].priority << " content=\""
              << calls[0].content << "\"\n";
    check(calls[0].type == MemoryType::Persona && calls[0].priority == 85 &&
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
  gMemory.init();
  const auto warmup = gEmbedding.embed("warmup", "query:");
  if (!warmup) {
    check(false, "embedding model loaded (run scripts/setup.sh)");
    gMemory.shutdown();
    return 1;
  }

  const auto gato = gEmbedding.embed("gato", "query:");
  const auto gatito = gEmbedding.embed("gatito", "query:");
  const auto computadora = gEmbedding.embed("computadora", "query:");
  const auto elGato = gEmbedding.embed("el gato", "query:");
  const auto theCat = gEmbedding.embed("the cat", "query:");
  const auto pescado = gEmbedding.embed("pescado", "query:");

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

  gMemory.shutdown();
  return fails == 0 ? 0 : 1;
}

struct FixturePair
{
  const char* memory;
  const char* query;
};

const FixturePair kFixture[] = {
    {"A Ana no le gusta el pescado", "¿qué no le gusta comer a Ana?"},
    {"Al abuelo le encanta el café con leche", "¿qué le encanta al abuelo?"},
    {"María trabaja de lunes a viernes en el hospital",
     "¿dónde trabaja María?"},
    {"El perro se llama Toby y duerme en el sofá", "¿cómo se llama el perro?"},
    {"La puerta de la cocina se atasca en invierno",
     "¿qué le pasa a la puerta de la cocina?"},
    {"El código del wifi es argus2026", "¿cuál es el wifi?"},
    {"Los domingos se riega el jardín por la mañana",
     "¿cuándo se riega el jardín?"},
    {"A Luis le da miedo la oscuridad", "¿a quién le da miedo la oscuridad?"},
    {"La abuela es alérgica a los frutos secos",
     "¿a qué es alérgica la abuela?"},
    {"El termostato se pone a 21 grados en invierno",
     "¿a cuánto se pone el termostato?"},
};

std::vector<int64_t> seedFixture()
{
  std::vector<int64_t> ids;
  for (const auto& pair : kFixture) {
    const int64_t id =
        captureAndSettle(kFixtureUser, "es",
                         std::string("recuerda que ") + pair.memory);
    if (id > 0)
      ids.push_back(id);
  }
  return ids;
}

void clearUserRows(int64_t userId)
{
  std::scoped_lock lock(gVecDb.mutex());
  sqlite3* db = gVecDb.handle();
  if (!db)
    return;
  SqliteStmt stmt;
  if (stmt.prepare(db, "SELECT id FROM memory_fact WHERE scope = 'user' AND "
                       "ref_id = ?")) {
    stmt.bindInt64(1, userId);
    while (stmt.step() == SQLITE_ROW) {
      SqliteStmt del;
      if (del.prepare(db, "DELETE FROM memory_vec WHERE memory_id = ?")) {
        del.bindInt64(1, stmt.columnInt64(0));
        del.step();
      }
    }
    stmt.finalize();
  }
  if (stmt.prepare(db, "DELETE FROM memory_fact WHERE scope = 'user' AND "
                       "ref_id = ?")) {
    stmt.bindInt64(1, userId);
    stmt.step();
    stmt.finalize();
  }
  if (stmt.prepare(db, "DELETE FROM memory_episode WHERE scope = 'user' AND "
                       "ref_id = ?")) {
    stmt.bindInt64(1, userId);
    stmt.step();
    stmt.finalize();
  }
}

int recallBench()
{
  ConfigService::load("config.toml");
  DbService::installExtensions();

  sqlite3* db = nullptr;
  {
    std::scoped_lock lock(gVecDb.mutex());
    db = gVecDb.handle();
  }
  if (!db)
    return 1;
  gVecDb.applySchema();

  gMemory.init();
  const auto warmup = gEmbedding.embed("warmup", "query:");
  if (!warmup) {
    check(false, "embedding model loaded (run scripts/setup.sh)");
    gMemory.shutdown();
    return 1;
  }

  clearUserRows(kFixtureUser);
  const auto ids = seedFixture();
  check(ids.size() == 10, "seeded 10 memories");
  gMemory.flushPending();

  std::vector<double> latencies;
  size_t totalChars = 0;
  int precisionHits = 0;
  for (size_t i = 0; i < ids.size(); ++i) {
    for (int run = 0; run < 3; ++run) {
      const auto t0 = std::chrono::steady_clock::now();
      const auto ctx = gMemory.recall({.userId = kFixtureUser,
                                       .text = kFixture[i].query,
                                       .lang = "es",
                                       .personIds = {}});
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
      latencies.push_back(ms);
      totalChars += ctx.prependText.size();
      const bool found = std::find(ctx.usedIds.begin(), ctx.usedIds.end(),
                                   ids[i]) != ctx.usedIds.end();
      if (found)
        ++precisionHits;
      if (run == 0 && !found)
        std::cout << "      miss: \"" << kFixture[i].query << "\" -> "
                  << kFixture[i].memory << "\n";
    }
  }

  const auto nomatch =
      gMemory.recall({.userId = kFixtureUser,
                      .text = "¿cuál es el código postal de la ciudad?",
                      .lang = "es",
                      .personIds = {}});
  check(nomatch.usedIds.empty(),
        "unrelated query injects nothing (precision gate)");

  std::sort(latencies.begin(), latencies.end());
  const double p50 = latencies[latencies.size() / 2];
  const double p95 = latencies[static_cast<size_t>(latencies.size() * 0.95)];
  std::cout << "[ok] recall bench: p50=" << p50 << "ms p95=" << p95
            << "ms avg_injected=" << (totalChars / 30 / 4)
            << " tokens, precision=" << precisionHits << "/30\n";
  check(precisionHits == 30, "recall precision 30/30");

  clearUserRows(kFixtureUser);
  {
    std::scoped_lock lock(gVecDb.mutex());
    sqlite3* db = gVecDb.handle();
    if (!db)
      return 1;
    SqliteStmt stmt;
    if (stmt.prepare(db, "SELECT COUNT(*) FROM memory_fact WHERE scope = "
                         "'user' AND ref_id = ?")) {
      stmt.bindInt64(1, kFixtureUser);
      if (stmt.step() == SQLITE_ROW)
        check(stmt.columnInt64(0) == 0, "bench cleanup");
      stmt.finalize();
    }
  }
  gMemory.shutdown();
  return fails == 0 ? 0 : 1;
}

int ftsDebug(const std::string& query)
{
  ConfigService::load("config.toml");
  DbService::installExtensions();
  {
    std::scoped_lock lock(gVecDb.mutex());
    if (!gVecDb.handle())
      return 1;
  }
  gVecDb.applySchema();
  gMemory.init();

  const auto ids = seedFixture();

  sqlite3* db = nullptr;
  {
    std::scoped_lock lock(gVecDb.mutex());
    db = gVecDb.handle();
  }

  std::string words;
  for (unsigned char c : query) {
    if (std::isalnum(c))
      words += static_cast<char>(std::tolower(c));
    else
      words += ' ';
  }
  std::vector<std::string> list;
  std::string cur;
  std::istringstream iss(words);
  while (iss >> cur) {
    if (cur.size() >= 3)
      list.push_back(cur);
  }
  std::string match;
  for (const auto& w : list) {
    if (!match.empty())
      match += " OR ";
    match += "\"" + w + "\"";
  }
  std::cout << "words: " << match << "\n";

  SqliteStmt stmt;
  const char* sql =
      "SELECT rowid, bm25(memory_fact_fts) AS r FROM memory_fact_fts "
      "WHERE memory_fact_fts MATCH ? ORDER BY r LIMIT 12";
  int rc = stmt.prepare(db, sql);
  std::cout << "fact_fts prepare rc=" << rc
            << (rc != SQLITE_OK ? sqlite3_errmsg(db) : "") << "\n";
  if (rc == SQLITE_OK) {
    stmt.bindText(1, match);
    int step = stmt.step();
    std::cout << "fact_fts step rc=" << step << "\n";
    while (step == SQLITE_ROW) {
      std::cout << "  hit rowid=" << stmt.columnInt64(0)
                << " bm25=" << stmt.columnDouble(1) << "\n";
      step = stmt.step();
    }
    stmt.finalize();
  }

  const auto vec = gEmbedding.embed(query, "query:");
  std::cout << "vec size=" << (vec ? vec->size() : 0) << "\n";
  if (vec) {
    std::string enc = "[";
    for (size_t i = 0; i < vec->size(); ++i) {
      if (i > 0)
        enc += ",";
      enc += std::to_string((*vec)[i]);
    }
    enc += "]";
    rc = stmt.prepare(db, "SELECT memory_id, distance FROM memory_vec "
                          "WHERE embedding MATCH ? AND partition = ? "
                          "ORDER BY distance LIMIT 12");
    std::cout << "vec prepare rc=" << rc
              << (rc != SQLITE_OK ? sqlite3_errmsg(db) : "") << "\n";
    if (rc == SQLITE_OK) {
      stmt.bindText(1, enc);
      stmt.bindText(2, "user:" + std::to_string(kFixtureUser));
      int step = stmt.step();
      std::cout << "vec step rc=" << step << "\n";
      while (step == SQLITE_ROW) {
        std::cout << "  vec-hit memory_id=" << stmt.columnInt64(0)
                  << " sim=" << (1.0 - stmt.columnDouble(1)) << "\n";
        step = stmt.step();
      }
      stmt.finalize();
    }
  }

  clearUserRows(kFixtureUser);
  gMemory.shutdown();
  return 0;
}

int recallDemo(const std::string& query)
{
  ConfigService::load("config.toml");
  DbService::installExtensions();
  {
    std::scoped_lock lock(gVecDb.mutex());
    if (!gVecDb.handle())
      return 1;
  }
  gVecDb.applySchema();
  gMemory.init();
  const auto warmup = gEmbedding.embed("warmup", "query:");
  if (!warmup) {
    std::cout << "embedding model not loaded (run scripts/setup.sh)\n";
    gMemory.shutdown();
    return 1;
  }

  clearUserRows(kFixtureUser);
  const auto ids = seedFixture();
  gMemory.flushPending();
  const auto ctx = gMemory.recall(
      {.userId = kFixtureUser, .text = query, .lang = "es", .personIds = {}});
  std::cout << "=== recall demo ===\n";
  std::cout << "query: " << query << "\n";
  std::cout << "injected (" << ctx.usedIds.size() << "):\n";
  if (!ctx.prependText.empty())
    std::cout << ctx.prependText << "\n";
  else
    std::cout << "prepend: (empty — gate rejected everything)\n";

  clearUserRows(kFixtureUser);
  gMemory.shutdown();
  return 0;
}

int simPair(const std::string& left, const std::string& right)
{
  ConfigService::load("config.toml");
  const auto a = gEmbedding.embed(left, "passage:");
  const auto b = gEmbedding.embed(right, "passage:");
  const auto q = gEmbedding.embed(left, "query:");
  if (!a || !b || !q) {
    std::cout << "embedding model not loaded (run scripts/setup.sh)\n";
    return 1;
  }
  std::cout << "passage/passage sim=" << cosine(*a, *b) << "\n";
  std::cout << "query/passage   sim=" << cosine(*q, *b) << "\n";
  return 0;
}

int scaleTest(int count)
{
  ConfigService::load("config.toml");
  DbService::installExtensions();
  {
    std::scoped_lock lock(gVecDb.mutex());
    if (!gVecDb.handle())
      return 1;
  }
  gVecDb.applySchema();
  gMemory.init();

  const char* names[] = {"ana",    "luis",  "carmen", "pedro", "marta",
                         "javier", "lucia", "diego",  "sofia", "hugo",
                         "elena",  "raul",  "nuria",  "ivan",  "clara",
                         "mateo",  "irene", "bruno",  "alba",  "sergio"};
  const char* foods[] = {"el pescado",  "el brocoli", "la cebolla",
                         "el queso",    "las setas",  "el pimiento",
                         "la coliflor", "el ajo",     "las anchoas",
                         "el hinojo"};
  const char* days[] = {"los lunes",   "los martes",  "los miercoles",
                        "los jueves",  "los viernes", "los sabados",
                        "los domingos"};
  const char* things[] = {"la bicicleta",  "el taladro",  "la escalera",
                          "el botiquin",   "la manta",    "el ventilador",
                          "la aspiradora", "el paraguas", "la linterna",
                          "el cargador"};
  const char* places[] = {"el garaje",  "el trastero", "la cocina",
                          "el altillo", "el sotano",   "la terraza",
                          "el pasillo", "el armario"};

  struct Seeded
  {
    int64_t id;
    std::string query;
    std::string expect;
  };
  std::vector<Seeded> seeded;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < count; ++i) {
    const std::string name = names[i % 20];
    const std::string owner = " de " + name;
    std::string text;
    std::string query;
    std::string expect;
    switch (i % 3) {
      case 0: {
        const std::string thing = things[(i / 20) % 10] + owner;
        const std::string place = places[(i / 60) % 8];
        text = "recuerda que " + thing + " esta en " + place;
        query = "¿donde esta " + thing + "?";
        expect = place;
        break;
      }
      case 1: {
        const std::string subject = "la reunion" + owner;
        const std::string day = days[(i / 60) % 7];
        text = "recuerda que " + subject + " es " + day;
        query = "¿cuando es la reunion" + owner + "?";
        expect = day;
        break;
      }
      default: {
        const std::string subject = "el gato" + owner;
        const std::string food = foods[(i / 60) % 10];
        text = "recuerda que a " + subject + " no le gusta " + food;
        query = "¿que no le gusta a " + subject + "?";
        expect = food;
        break;
      }
    }
    const int64_t id = captureAndSettle(kFixtureUser, "es", text);
    if (id > 0 && i % 7 == 0)
      seeded.push_back({.id = id, .query = query, .expect = expect});
  }
  gMemory.flushPending();
  const double seedMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();

  int stored = 0;
  {
    std::scoped_lock lock(gVecDb.mutex());
    SqliteStmt stmt;
    if (stmt.prepare(gVecDb.handle(),
                     "SELECT COUNT(*) FROM memory_fact WHERE scope = 'user' "
                     "AND ref_id = ? AND valid_to = 0")) {
      stmt.bindInt64(1, kFixtureUser);
      if (stmt.step() == SQLITE_ROW)
        stored = stmt.columnInt(0);
    }
  }
  std::cout << "stored=" << stored << " (seed " << seedMs << " ms)\n";

  int found = 0;
  int rightContent = 0;
  size_t injectedTotal = 0;
  size_t injectedMax = 0;
  std::vector<double> latencies;
  for (const auto& probe : seeded) {
    const auto t1 = std::chrono::steady_clock::now();
    const auto ctx = gMemory.recall({.userId = kFixtureUser,
                                     .text = probe.query,
                                     .lang = "es",
                                     .personIds = {}});
    latencies.push_back(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t1)
                            .count());
    injectedTotal += ctx.usedIds.size();
    injectedMax = std::max(injectedMax, ctx.usedIds.size());
    if (std::find(ctx.usedIds.begin(), ctx.usedIds.end(), probe.id) !=
        ctx.usedIds.end())
      ++found;
    if (ctx.prependText.find(probe.expect) != std::string::npos)
      ++rightContent;
  }

  int chatLeak = 0;
  const char* smalltalk[] = {"hola",
                             "gracias",
                             "buenos dias",
                             "Hola Argos, ¿como estas?",
                             "¿que tal tu dia?",
                             "¿me cuentas un chiste?",
                             "que tengas buen dia",
                             "adios"};
  for (const char* line : smalltalk) {
    const auto ctx = gMemory.recall(
        {.userId = kFixtureUser, .text = line, .lang = "es", .personIds = {}});
    if (!ctx.usedIds.empty()) {
      ++chatLeak;
      std::cout << "  LEAK \"" << line << "\" -> " << ctx.usedIds.size()
                << " memorias\n";
    }
  }

  std::sort(latencies.begin(), latencies.end());
  const double p50 = latencies.empty() ? 0 : latencies[latencies.size() / 2];
  const double p95 =
      latencies.empty()
          ? 0
          : latencies[std::min(latencies.size() - 1,
                               static_cast<size_t>(latencies.size() * 0.95))];
  const double queries = static_cast<double>(seeded.size());
  std::cout << "queries=" << seeded.size() << " target-recalled=" << found
            << " (" << (100.0 * found / queries) << "%)"
            << " right-content=" << rightContent << "\n";
  std::cout << "injected avg=" << (queries > 0 ? injectedTotal / queries : 0.0)
            << " max=" << injectedMax << "\n";
  std::cout << "recall p50=" << p50 << " ms p95=" << p95 << " ms\n";
  std::cout << "smalltalk leaks=" << chatLeak << "/8\n";

  clearUserRows(kFixtureUser);
  gMemory.shutdown();
  return 0;
}

int vecRows(const std::string& partition)
{
  ConfigService::load("config.toml");
  DbService::installExtensions();
  std::scoped_lock lock(gVecDb.mutex());
  sqlite3* db = gVecDb.handle();
  if (!db)
    return 1;
  SqliteStmt stmt;
  if (!stmt.prepare(db, "SELECT memory_id, view FROM memory_vec WHERE "
                        "partition = ?"))
    return 1;
  stmt.bindText(1, partition);
  int orphans = 0;
  int total = 0;
  while (stmt.step() == SQLITE_ROW) {
    const int64_t memoryId = stmt.columnInt64(0);
    const int view = stmt.columnInt(1);
    SqliteStmt fact;
    bool alive = false;
    std::string canonical;
    if (fact.prepare(db, "SELECT canonical FROM memory_fact WHERE id = ?")) {
      fact.bindInt64(1, memoryId);
      if (fact.step() == SQLITE_ROW) {
        alive = true;
        canonical = fact.columnText(0);
      }
    }
    ++total;
    if (!alive)
      ++orphans;
    std::cout << "  memory_id=" << memoryId << " view=" << view << " "
              << (alive ? canonical : "(ORPHAN: fact deleted)") << "\n";
  }
  std::cout << "rows=" << total << " orphans=" << orphans << "\n";
  return 0;
}

int recallUser(int64_t userId, const std::string& query)
{
  ConfigService::load("config.toml");
  DbService::installExtensions();
  {
    std::scoped_lock lock(gVecDb.mutex());
    if (!gVecDb.handle())
      return 1;
  }
  gVecDb.applySchema();
  gMemory.init();

  const auto ctx = gMemory.recall(
      {.userId = userId, .text = query, .lang = "es", .personIds = {}});
  std::cout << "=== recall user " << userId << " ===\n";
  std::cout << "query: \"" << query << "\"\n";
  std::cout << "injected (" << ctx.usedIds.size() << "):\n";
  std::cout << (ctx.prependText.empty() ? "  (nothing)\n" : ctx.prependText);
  gMemory.shutdown();
  return 0;
}

int simScan(const std::string& query)
{
  ConfigService::load("config.toml");
  DbService::installExtensions();
  {
    std::scoped_lock lock(gVecDb.mutex());
    if (!gVecDb.handle())
      return 1;
  }
  gVecDb.applySchema();
  gMemory.init();
  const auto q = gEmbedding.embed(query, "query:");
  if (!q) {
    std::cout << "embedding model not loaded (run scripts/setup.sh)\n";
    gMemory.shutdown();
    return 1;
  }

  clearUserRows(kFixtureUser);
  seedFixture();
  gMemory.flushPending();

  std::vector<std::pair<float, std::string>> scored;
  {
    std::scoped_lock lock(gVecDb.mutex());
    sqlite3* db = gVecDb.handle();
    SqliteStmt stmt;
    if (!stmt.prepare(db, "SELECT canonical FROM memory_fact WHERE ref_id = ? "
                          "AND valid_to = 0"))
      return 1;
    stmt.bindInt64(1, kFixtureUser);
    std::vector<std::string> facts;
    while (stmt.step() == SQLITE_ROW)
      facts.push_back(stmt.columnText(0));
    stmt.finalize();
    for (const auto& fact : facts) {
      const auto p = gEmbedding.embed(fact, "passage:");
      if (p)
        scored.push_back({cosine(*q, *p), fact});
    }
  }
  std::sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  float sum = 0.0F;
  for (const auto& row : scored)
    sum += row.first;
  const float mean = scored.empty() ? 0.0F : sum / scored.size();

  std::cout << "=== sim scan ===\nquery: \"" << query << "\"\n";
  for (const auto& row : scored)
    std::cout << "  " << row.first << "  " << row.second << "\n";
  if (!scored.empty())
    std::cout << "  max=" << scored.front().first << " mean=" << mean
              << " margin=" << (scored.front().first - mean) << "\n";
  gMemory.shutdown();
  return 0;
}

int simsCheck()
{
  ConfigService::load("config.toml");
  DbService::installExtensions();
  {
    std::scoped_lock lock(gVecDb.mutex());
    if (!gVecDb.handle())
      return 1;
  }
  gVecDb.applySchema();
  gMemory.init();
  const auto warmup = gEmbedding.embed("warmup", "query:");
  if (!warmup) {
    std::cout << "embedding model not loaded (run scripts/setup.sh)\n";
    gMemory.shutdown();
    return 1;
  }

  clearUserRows(kFixtureUser);
  const auto ids = seedFixture();
  gMemory.flushPending();

  const auto queryVecs = [&] {
    std::vector<std::optional<std::vector<float>>> out;
    for (const auto& pair : kFixture)
      out.push_back(gEmbedding.embed(pair.query, "query:"));
    return out;
  }();

  const auto all = [&] {
    std::vector<std::pair<int64_t, std::string>> rows;
    std::scoped_lock lock(gVecDb.mutex());
    sqlite3* db = gVecDb.handle();
    if (!db)
      return rows;
    SqliteStmt stmt;
    if (stmt.prepare(db, "SELECT id, value FROM memory_fact WHERE scope = "
                         "'user' AND ref_id = ? AND valid_to = 0")) {
      stmt.bindInt64(1, kFixtureUser);
      while (stmt.step() == SQLITE_ROW)
        rows.emplace_back(stmt.columnInt64(0), stmt.columnText(1));
      stmt.finalize();
    }
    return rows;
  }();
  for (size_t i = 0; i < ids.size(); ++i) {
    const auto& q = queryVecs[i];
    if (!q) {
      std::cout << kFixture[i].query << " -> (no vec)\n";
      continue;
    }
    std::cout << kFixture[i].query << "\n";
    for (const auto& entry : all) {
      if (entry.first != ids[i])
        continue;
      const auto passage = gEmbedding.embed(entry.second, "passage:");
      if (passage)
        std::cout << "    target id=" << entry.first
                  << " sim=" << cosine(*q, *passage) << "\n";
      break;
    }
  }

  clearUserRows(kFixtureUser);
  gMemory.shutdown();
  return 0;
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

int vecGateTest()
{
  ConfigService::load("config.toml");
  DbService::installExtensions();
  {
    std::scoped_lock lock(gVecDb.mutex());
    if (!gVecDb.handle())
      return 1;
  }
  gVecDb.applySchema();
  gMemory.init();
  const auto warmup = gEmbedding.embed("warmup", "query:");
  if (!warmup) {
    check(false, "embedding model loaded (run scripts/setup.sh)");
    gMemory.shutdown();
    return 1;
  }

  struct Paraphrase
  {
    const char* memory;
    const char* query;
  };
  const Paraphrase pairs[] = {
      {"la cena se sirve a las ocho", "¿a qué hora comemos?"},
      {"al perro no le gusta bañarse", "¿qué odia el can?"},
      {"mi hermana viene los domingos", "¿cuándo viene mi hermana?"},
  };

  clearUserRows(kFixtureUser);
  std::vector<int64_t> seeded;
  for (const auto& pair : pairs) {
    const int64_t id =
        captureAndSettle(kFixtureUser, "es",
                         std::string("recuerda que ") + pair.memory);
    check(id > 0, "seeded: \"" + std::string(pair.memory) + "\"");
    seeded.push_back(id);
  }
  gMemory.flushPending();

  int hits = 0;
  for (size_t i = 0; i < std::size(pairs); ++i) {
    const auto ctx = gMemory.recall({.userId = kFixtureUser,
                                     .text = pairs[i].query,
                                     .lang = "es",
                                     .personIds = {}});
    const bool found =
        seeded[i] > 0 && std::find(ctx.usedIds.begin(), ctx.usedIds.end(),
                                   seeded[i]) != ctx.usedIds.end();
    std::cout << "      \"" << pairs[i].query << "\" -> "
              << (found ? "recalled" : "MISS") << "\n";
    if (found)
      ++hits;
  }

  const auto noise = gMemory.recall({.userId = kFixtureUser,
                                     .text = "¿cuál es el clima en marte?",
                                     .lang = "es",
                                     .personIds = {}});
  check(noise.usedIds.empty(), "unrelated query injects nothing");

  for (const char* smalltalk :
       {"hola", "gracias", "buenos días", "Hola Argos, ¿cómo estás?",
        "¿qué tal tu día?", "oye argus, ¿todo bien?"}) {
    const auto chat = gMemory.recall({.userId = kFixtureUser,
                                      .text = smalltalk,
                                      .lang = "es",
                                      .personIds = {}});
    check(chat.usedIds.empty(),
          std::string("smalltalk injects nothing: \"") + smalltalk + "\"");
  }

  for (const auto& pair : pairs) {
    const auto hit = gMemory.recall({.userId = kFixtureUser,
                                     .text = pair.query,
                                     .lang = "es",
                                     .personIds = {}});
    check(hit.usedIds.size() <= 2,
          std::string("targeted query stays selective: \"") + pair.query +
              "\" (" + std::to_string(hit.usedIds.size()) + " facts)");
  }

  check(hits == 3, "paraphrase recall gate 3/3");
  check(hits >= 2, "semantic layer functional (>= 2/3)");

  clearUserRows(kFixtureUser);
  gMemory.shutdown();
  return fails == 0 ? 0 : 1;
}

int dedupBench()
{
  ConfigService::load("config.toml");
  DbService::installExtensions();
  {
    std::scoped_lock lock(gVecDb.mutex());
    if (!gVecDb.handle())
      return 1;
  }
  gVecDb.applySchema();
  gMemory.init();
  const auto warmup = gEmbedding.embed("warmup", "query:");
  if (!warmup) {
    check(false, "embedding model loaded (run scripts/setup.sh)");
    gMemory.shutdown();
    return 1;
  }

  clearUserRows(kFixtureUser);
  const int64_t a =
      captureAndSettle(kFixtureUser, "es",
                       "recuerda que el wifi de casa se llama argus2026");
  const int64_t b = captureAndSettle(
      kFixtureUser, "es",
      "recuerda que la red domestica tiene la clave argus2026");
  check(a > 0 && b > 0, "two rows saved");
  check(a != b, "lexical dedup did not merge the paraphrases");

  gMemory.flushPending();

  {
    std::scoped_lock lock(gVecDb.mutex());
    sqlite3* db = gVecDb.handle();
    if (!db)
      return 1;
    SqliteStmt stmt;
    if (!stmt.prepare(db, "SELECT memory_id FROM memory_vec WHERE "
                          "memory_id IN (?, ?)")) {
      check(false, "vec dedup query prepare");
      return 1;
    }
    stmt.bindInt64(1, a);
    stmt.bindInt64(2, b);
    std::set<int64_t> vecIds;
    while (stmt.step() == SQLITE_ROW)
      vecIds.insert(stmt.columnInt64(0));
    stmt.finalize();
    check(vecIds.size() == 1,
          "vector dedup merged the duplicate (1 fact indexed, saw " +
              std::to_string(vecIds.size()) + ")");
    if (vecIds.size() == 1)
      check(*vecIds.begin() == a, "survivor vec row is the first fact");
  }

  clearUserRows(kFixtureUser);
  gMemory.shutdown();
  return fails == 0 ? 0 : 1;
}

int purgeCmd(const std::string& scope, int64_t refId)
{
  ConfigService::load("config.toml");
  DbService::installExtensions();
  {
    std::scoped_lock lock(gVecDb.mutex());
    if (!gVecDb.handle())
      return 1;
  }
  gVecDb.applySchema();
  gMemory.init();
  {
    std::scoped_lock lock(gVecDb.mutex());
    sqlite3* db = gVecDb.handle();
    if (!db)
      return 1;
    SqliteStmt stmt;
    if (stmt.prepare(db, "SELECT id FROM memory_fact WHERE scope = ? AND "
                         "ref_id = ?")) {
      stmt.bindText(1, scope);
      stmt.bindInt64(2, refId);
      while (stmt.step() == SQLITE_ROW) {
        SqliteStmt del;
        if (del.prepare(db, "DELETE FROM memory_vec WHERE memory_id = ?")) {
          del.bindInt64(1, stmt.columnInt64(0));
          del.step();
        }
      }
      stmt.finalize();
    }
    if (stmt.prepare(db, "DELETE FROM memory_fact WHERE scope = ? AND "
                         "ref_id = ?")) {
      stmt.bindText(1, scope);
      stmt.bindInt64(2, refId);
      stmt.step();
      stmt.finalize();
    }
    if (stmt.prepare(db, "DELETE FROM memory_episode WHERE scope = ? AND "
                         "ref_id = ?")) {
      stmt.bindText(1, scope);
      stmt.bindInt64(2, refId);
      stmt.step();
      stmt.finalize();
    }
  }
  gMemory.shutdown();
  std::cout << "purged " << scope << ":" << refId << "\n";
  return 0;
}

int seedCmd(int64_t userId, const std::string& text)
{
  ConfigService::load("config.toml");
  DbService::installExtensions();
  {
    std::scoped_lock lock(gVecDb.mutex());
    if (!gVecDb.handle())
      return 1;
  }
  gVecDb.applySchema();
  gMemory.init();
  const auto warmup = gEmbedding.embed("warmup", "query:");
  if (!warmup) {
    std::cout << "embedding model not loaded (run scripts/setup.sh)\n";
    gMemory.shutdown();
    return 1;
  }

  int64_t id = captureAndSettle(userId, "es", text);
  if (id <= 0)
    id = captureAndSettle(userId, "es", "recuerda que " + text);
  gMemory.flushPending();
  std::cout << "seeded id=" << id << " (user " << userId << ")\n";
  gMemory.shutdown();
  return id > 0 ? 0 : 1;
}

int graphRecallTest()
{
  const std::string dbPath = "database/recall-test.db";
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  SqliteGraph graph;
  graph.open(dbPath);
  EntityResolver resolver(graph);
  GraphRecall recall(graph, resolver, gEmbedding, gVecDb);

  const int64_t hermana = graph.createEntity({.kind = "person",
                                              .canonical = "hermana",
                                              .lang = "es",
                                              .personId = std::nullopt});
  const int64_t rodrigo = graph.createEntity({.kind = "person",
                                              .canonical = "rodrigo",
                                              .lang = "es",
                                              .personId = std::nullopt});
  const int64_t usuario = graph.createEntity({.kind = "person",
                                              .canonical = "usuario",
                                              .lang = "es",
                                              .personId = std::nullopt});
  graph.addAlias({.entityId = hermana,
                  .surface = "mi hermana",
                  .norm = "mi hermana",
                  .lang = "es",
                  .personFrame = "first",
                  .confidence = 0.95F});
  graph.addAlias({.entityId = hermana,
                  .surface = "tu hermana",
                  .norm = "tu hermana",
                  .lang = "es",
                  .personFrame = "second",
                  .confidence = 0.9F});
  graph.addAlias({.entityId = rodrigo,
                  .surface = "rodrigo",
                  .norm = "rodrigo",
                  .lang = "es",
                  .personFrame = "none",
                  .confidence = 0.95F});
  graph.addAlias({.entityId = usuario,
                  .surface = "yo",
                  .norm = "yo",
                  .lang = "es",
                  .personFrame = "first",
                  .confidence = 0.95F});
  graph.addAlias({.entityId = usuario,
                  .surface = "tu",
                  .norm = "tu",
                  .lang = "es",
                  .personFrame = "second",
                  .confidence = 0.9F});
  graph.upsertFact({.entityId = hermana,
                    .predicate = "visits_on",
                    .value = "domingos",
                    .canonical = "visita los domingos",
                    .type = "schedule",
                    .priority = 85,
                    .confidence = 0.9F,
                    .lang = "es",
                    .scope = "user",
                    .refId = kFixtureUser,
                    .now = 1000,
                    .sourceId = std::nullopt});
  graph.upsertFact({.entityId = rodrigo,
                    .predicate = "dislikes",
                    .value = "el pescado",
                    .canonical = "no le gusta el pescado",
                    .type = "persona",
                    .priority = 80,
                    .confidence = 0.9F,
                    .lang = "es",
                    .scope = "user",
                    .refId = kFixtureUser,
                    .now = 1000,
                    .sourceId = std::nullopt});
  graph.upsertFact({.entityId = usuario,
                    .predicate = "likes",
                    .value = "el cafe sin azucar",
                    .canonical = "le gusta el cafe sin azucar",
                    .type = "persona",
                    .priority = 80,
                    .confidence = 0.9F,
                    .lang = "es",
                    .scope = "user",
                    .refId = kFixtureUser,
                    .now = 1000,
                    .sourceId = std::nullopt});

  auto r1 = recall.recall({.text = "cuando viene mi hermana",
                           .lang = "es",
                           .scope = "user",
                           .refId = kFixtureUser,
                           .maxHops = 1,
                           .limit = 8,
                           .addresseeEntityId = usuario});
  bool hermanaHit = false;
  bool tuForm = false;
  for (const auto& h : r1.hits) {
    if (h.entityId == hermana)
      hermanaHit = true;
    if (h.rendered.find("tu hermana") != std::string::npos)
      tuForm = true;
  }
  check(hermanaHit, "recall v2: hermana fact found");
  check(tuForm, "recall v2: rendered via second-person frame");
  bool coffeeAttributed = true;
  for (const auto& h : r1.hits)
    if (h.canonical.find("cafe") != std::string::npos && h.entityId != usuario)
      coffeeAttributed = false;
  check(coffeeAttributed,
        "recall v2: coffee attributed to the user, not others");

  auto r2 = recall.recall({.text = "que no le gusta a rodrigo",
                           .lang = "es",
                           .scope = "user",
                           .refId = kFixtureUser,
                           .maxHops = 1,
                           .limit = 8,
                           .addresseeEntityId = usuario});
  bool rodrigoHit = false;
  for (const auto& h : r2.hits)
    if (h.entityId == rodrigo)
      rodrigoHit = true;
  check(rodrigoHit, "recall v2: rodrigo fish fact found");
  bool fishOnly = true;
  for (const auto& h : r2.hits)
    if (h.canonical.find("cafe") != std::string::npos)
      fishOnly = false;
  check(fishOnly, "recall v2: rodrigo query excludes coffee");

  auto r3 = recall.recall({.text = "que me gusta tomar",
                           .lang = "es",
                           .scope = "user",
                           .refId = kFixtureUser,
                           .maxHops = 1,
                           .limit = 8,
                           .addresseeEntityId = usuario});
  bool userHit = false;
  for (const auto& h : r3.hits)
    if (h.entityId == usuario && h.canonical.find("cafe") != std::string::npos)
      userHit = true;
  check(userHit, "recall v2: user coffee fact attributed to the user");

  auto r4 =
      recall.recall({.text = "cuando viene mi hermana y que le gusta a rodrigo",
                     .lang = "es",
                     .scope = "user",
                     .refId = kFixtureUser,
                     .maxHops = 1,
                     .limit = 8,
                     .addresseeEntityId = usuario});
  bool both = false;
  bool h = false;
  bool r = false;
  for (const auto& hit : r4.hits) {
    if (hit.entityId == hermana)
      h = true;
    if (hit.entityId == rodrigo)
      r = true;
  }
  both = h && r;
  check(both, "recall v2: multi-memory query returns both facts");

  graph.close();
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  return fails == 0 ? 0 : 1;
}

int toolsTest()
{
  const std::string dbPath = "database/tools-test.db";
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  ConfigService::load("config.toml");

  SqliteGraph graph;
  graph.open(dbPath);
  graph.migrateLegacy();

  EntityResolver resolver(graph);
  PhraseCatalog catalog(graph);
  catalog.build();
  RuleParser ruleParser(catalog);
  MemoryFormation formation(graph, resolver, ruleParser);
  GraphRecall recall(graph, resolver, gEmbedding, gVecDb);
  ToolRegistry& registry = ToolRegistry::instance();
  {
    tools::ToolCall remember;
    remember.name = "memory.remember";
    remember.arguments["subject"] = "el termostato";
    remember.arguments["predicate"] = "is_set_to";
    remember.arguments["value"] = "21 grados";
    remember.arguments["type"] = "attribute";
    remember.context.userId = kFixtureUser;
    remember.context.lang = "es";
    const auto result = [&] {
      tools::ToolDescriptor descriptor;
      descriptor.name = "memory.remember";
      descriptor.accessTable = TableName::Memory;
      descriptor.accessPermission = RolePermission::Create;
      descriptor.arguments = {{"subject", "string", true, {}, ""},
                              {"predicate", "string", true, {}, ""},
                              {"value", "string", true, {}, ""},
                              {"type",
                               "enum",
                               true,
                               {"persona", "preference", "schedule",
                                "instruction", "attribute"},
                               ""}};
      descriptor.handler = [&](const tools::ToolCall& call) {
        const auto formed = formation.observe(
            {.channel = "tool_result",
             .text = call.arguments.get("subject", "").asString() + " " +
                     call.arguments.get("predicate", "").asString() + " " +
                     call.arguments.get("value", "").asString(),
             .actor = "",
             .at = std::time(nullptr),
             .userId = call.context.userId,
             .lang = call.context.lang,
             .sessionId = call.context.sessionId,
             .entitiesHint = {}},
            call);
        tools::ToolResult r;
        if (!formed) {
          r.output = "fail";
          return r;
        }
        r.ok = true;
        r.output = "saved " + std::to_string(formed->factId);
        return r;
      };
      registry.registerTool(descriptor);
      return ToolExecutor(registry).execute(remember, UserRole::Resident);
    }();
    check(result.ok && result.output.find("saved") != std::string::npos,
          "tools: memory.remember executes for Resident");
  }

  tools::ToolCall recallCall;
  recallCall.name = "memory.recall";
  recallCall.arguments["query"] = "el termostato";
  recallCall.context.userId = kFixtureUser;
  recallCall.context.lang = "es";
  tools::ToolDescriptor recallDescriptor;
  recallDescriptor.name = "memory.recall";
  recallDescriptor.accessTable = TableName::Memory;
  recallDescriptor.accessPermission = RolePermission::Read;
  recallDescriptor.arguments = {{"query", "string", true, {}, ""}};
  recallDescriptor.handler = [&](const tools::ToolCall& call) {
    tools::ToolResult r;
    const auto hit =
        recall.recall({.text = call.arguments.get("query", "").asString(),
                       .lang = call.context.lang,
                       .scope = "user",
                       .refId = call.context.userId,
                       .maxHops = 1,
                       .limit = 8,
                       .addresseeEntityId = 0});
    if (hit.hits.empty()) {
      r.output = "empty";
      return r;
    }
    r.ok = true;
    r.output = hit.block;
    return r;
  };
  registry.registerTool(recallDescriptor);
  ToolExecutor executor(registry);
  const auto r = executor.execute(recallCall, UserRole::Resident);
  check(r.ok && r.output.find("termostato") != std::string::npos,
        "tools: memory.recall returns the stored fact");

  const auto denied = executor.execute(recallCall, UserRole::Guest);
  check(!denied.ok &&
            denied.output.find("permission denied") != std::string::npos,
        "tools: Guest denied (role_access)");

  tools::ToolCall badCall = recallCall;
  badCall.arguments.removeMember("query");
  const auto invalid = executor.execute(badCall, UserRole::Resident);
  check(!invalid.ok &&
            invalid.output.find("missing required") != std::string::npos,
        "tools: validator rejects missing argument");

  tools::ToolCall unknownCall;
  unknownCall.name = "no.such.tool";
  const auto unknown = executor.execute(unknownCall, UserRole::Owner);
  check(!unknown.ok && unknown.output.find("unknown tool") != std::string::npos,
        "tools: unknown tool rejected");

  graph.close();
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  return fails == 0 ? 0 : 1;
}

int entityTest()
{
  const std::string dbPath = "database/entity-test.db";
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  SqliteGraph graph;
  graph.open(dbPath);
  graph.applySchema();

  {
    std::scoped_lock lock(graph.mutex());
    SqliteStmt stmt;
    if (stmt.prepare(
            graph.handle(),
            "INSERT INTO memory_entity (kind, canonical, lang, created_at, "
            "updated_at, person_id) VALUES ('person', 'hermana', 'es', 1, 1, "
            "NULL)"))
      stmt.step();
    const int64_t hermana = sqlite3_last_insert_rowid(graph.handle());
    check(hermana > 0, "entity test: entity seeded");
    if (stmt.prepare(
            graph.handle(),
            "INSERT INTO memory_alias (entity_id, surface, norm, lang, "
            "person_frame, confidence) VALUES (?, 'mi hermana', 'mi hermana', "
            "'es', 'first', 0.95)")) {
      stmt.bindInt64(1, hermana);
      stmt.step();
    }
    if (stmt.prepare(
            graph.handle(),
            "INSERT INTO memory_alias (entity_id, surface, norm, lang, "
            "person_frame, confidence) VALUES (?, 'tu hermana', 'tu hermana', "
            "'es', 'second', 0.9)")) {
      stmt.bindInt64(1, hermana);
      stmt.step();
    }
    if (stmt.prepare(
            graph.handle(),
            "CREATE TABLE IF NOT EXISTS person (id INTEGER PRIMARY KEY "
            "AUTOINCREMENT, name TEXT NOT NULL DEFAULT '', alias TEXT NOT NULL "
            "DEFAULT '', deleted_at INTEGER)"))
      stmt.step();
    if (stmt.prepare(graph.handle(),
                     "INSERT INTO person (name, alias) VALUES ('Ana', 'ana')"))
      stmt.step();
    if (stmt.prepare(
            graph.handle(),
            "CREATE TABLE IF NOT EXISTS camera (id INTEGER PRIMARY KEY "
            "AUTOINCREMENT, name TEXT NOT NULL, ip TEXT NOT NULL DEFAULT '', "
            "deleted_at INTEGER)"))
      stmt.step();
    if (stmt.prepare(
            graph.handle(),
            "INSERT INTO camera (name, ip) VALUES ('Entrada', '192.168.1.10')"))
      stmt.step();
    if (stmt.prepare(graph.handle(),
                     "CREATE TABLE IF NOT EXISTS zone (id INTEGER PRIMARY KEY "
                     "AUTOINCREMENT, camera_id INTEGER, name TEXT NOT NULL, "
                     "points TEXT NOT NULL DEFAULT '[]')"))
      stmt.step();
    if (stmt.prepare(graph.handle(),
                     "INSERT INTO zone (camera_id, name, points) VALUES "
                     "(1, 'Patio', '[]')"))
      stmt.step();
    if (stmt.prepare(
            graph.handle(),
            "CREATE TABLE IF NOT EXISTS camera_stream (id INTEGER PRIMARY KEY "
            "AUTOINCREMENT, camera_id INTEGER, label TEXT NOT NULL DEFAULT '', "
            "url TEXT NOT NULL DEFAULT '')"))
      stmt.step();
    if (stmt.prepare(graph.handle(),
                     "INSERT INTO camera_stream (camera_id, label, url) "
                     "VALUES (1, '', '')"))
      stmt.step();
  }

  EntityResolver resolver(graph);
  resolver.build();
  check(true, "entity test: trie built");

  const auto hits1 = resolver.resolve("mi hermana viene los domingos");
  bool firstFrame = false;
  for (const auto& h : hits1)
    if (h.personFrame == "first" && h.kind == "person")
      firstFrame = true;
  check(firstFrame, "entity test: 'mi hermana' resolves (frame first)");

  const auto hits2 = resolver.resolve("tu hermana");
  bool secondFrame = false;
  for (const auto& h : hits2)
    if (h.personFrame == "second")
      secondFrame = true;
  check(secondFrame, "entity test: 'tu hermana' resolves (frame second)");

  const auto hits3 =
      resolver.resolve("le dije a Ana que apagase el termostato");
  bool ana = false;
  for (const auto& h : hits3) {
    if (h.catalog == "person" && h.surface == "Ana")
      ana = true;
  }
  check(ana, "entity test: catalog person 'Ana' resolved");

  const auto hits3b = resolver.resolve("abre la camara de la entrada");
  bool entrada = false;
  for (const auto& h : hits3b) {
    if (h.catalog == "camera" && h.surface == "Entrada")
      entrada = true;
  }
  check(entrada, "entity test: catalog camera 'Entrada' resolved");

  const auto hits4 = resolver.resolve("la camara del patio no funciona");
  bool patio = false;
  for (const auto& h : hits4)
    if (h.catalog == "zone" && h.surface == "Patio")
      patio = true;
  check(patio, "entity test: zone 'Patio' resolved");

  graph.close();
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  return fails == 0 ? 0 : 1;
}

int formationTest()
{
  ConfigService::load("config.toml");
  const std::string dbPath = "database/formation-test.db";
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  SqliteGraph graph;
  graph.open(dbPath);
  graph.applySchema();

  EntityResolver resolver(graph);
  PhraseCatalog catalog(graph);
  catalog.build();
  RuleParser ruleParser(catalog);
  MemoryFormation formation(graph, resolver, ruleParser);
  const auto phrase =
      formation.observe({.channel = "user_turn",
                         .text = "recuerda que mi hermana viene los domingos",
                         .actor = "",
                         .at = 1000,
                         .userId = kFixtureUser,
                         .lang = "es",
                         .sessionId = "s1",
                         .entitiesHint = {}});
  check(phrase.has_value() && phrase->factId > 0, "formation: phrase captured");
  check(phrase.has_value() && phrase->predicate == "nota",
        "formation: phrase path falls back to 'nota' with no extractor");
  check(phrase.has_value() && phrase->subjectEntityId > 0,
        "formation: subject entity resolved");

  ExtractionService extractModel;
  TieredExtractor tiered(extractModel);
  formation.setExtractor(&tiered);
  const auto viaExtractor = formation.observe(
      {.channel = "user_turn",
       .text = "recuerda que a mi prima no le gusta el pescado",
       .actor = "",
       .at = 1050,
       .userId = kFixtureUser,
       .lang = "es",
       .sessionId = "s1",
       .entitiesHint = {}});
  check(viaExtractor.has_value() && viaExtractor->factId > 0,
        "formation: extractor path captured");
  check(viaExtractor.has_value() && viaExtractor->predicate != "nota" &&
            !viaExtractor->predicate.empty(),
        "formation: extractor path yields a real predicate (got '" +
            (viaExtractor ? viaExtractor->predicate : std::string("none")) +
            "')");
  check(viaExtractor.has_value() &&
            viaExtractor->canonical.find("dislikes") == std::string::npos,
        "formation: canonical stays natural language (no english label)");

  const char* kNoCapture[] = {
      "hola argos, como estas",
      "que es lo que no le gustaba a rodrigo",
      "quisiera saber como reaccionarias ante un intruso",
      "buenos dias",
  };
  for (const char* text : kNoCapture) {
    const auto none = formation.observe({.channel = "user_turn",
                                         .text = text,
                                         .actor = "",
                                         .at = 1060,
                                         .userId = kFixtureUser,
                                         .lang = "es",
                                         .sessionId = "s1",
                                         .entitiesHint = {}});
    check(!none.has_value(),
          std::string("formation: NOT captured -> \"") + text + "\"");
  }

  struct SalientCase
  {
    const char* text;
    bool shouldCapture;
  };
  const SalientCase kSalient[] = {
      {"a mi prima no le gusta la cebolla", true},
      {"mi hermano entrena los martes", true},
      {"el tecnico revisa la caldera en octubre", true},
      {"eh, bien, si, argus queria saber que no le gustaba a rodrigo", false},
      {"que no le gustaba a rodrigo", false},
      {"que comida no le gusta a rodrigo", false},
      {"cuando entrena mi hermano", false},
      {"quien revisa la caldera", false},
      {"la pregunta va mas a que si alguien por ejemplo yo estoy en mi casa "
       "y si alguien intenta entrar y es una persona no autorizada como tu "
       "reaccionarias",
       false},
      {"hola argos como estas", false},
      {"mira, quisiera saber, bueno quisiera que me recuerdes acerca de que "
       "a rodrigo no le gusta el pescado ok",
       false},
  };
  for (const auto& c : kSalient) {
    const auto out = formation.observe({.channel = "user_turn",
                                        .text = c.text,
                                        .actor = "",
                                        .at = 1070,
                                        .userId = kFixtureUser,
                                        .lang = "es",
                                        .sessionId = "s1",
                                        .entitiesHint = {},
                                        .allowModel = true,
                                        .salient = true});
    check(out.has_value() == c.shouldCapture,
          std::string("formation: salient+complete=") +
              (c.shouldCapture ? "capture" : "reject") + " -> \"" + c.text +
              "\"");
  }

  tools::ToolCall remember;
  remember.name = "memory.remember";
  remember.arguments["subject"] = "el termostato";
  remember.arguments["predicate"] = "is_set_to";
  remember.arguments["value"] = "21 grados";
  remember.arguments["type"] = "attribute";
  remember.arguments["confidence"] = 0.9;
  const auto tool = formation.observe({.channel = "user_turn",
                                       .text = "pon el termostato a 21 grados",
                                       .actor = "",
                                       .at = 1100,
                                       .userId = kFixtureUser,
                                       .lang = "es",
                                       .sessionId = "s1",
                                       .entitiesHint = {}},
                                      remember);
  check(tool.has_value() && tool->factId > 0, "formation: tool call captured");
  check(tool.has_value() && tool->predicate == "is_set_to" &&
            tool->canonical.find("21 grados") != std::string::npos,
        "formation: structured predicate + canonical");

  const auto hits = resolver.resolve("el termostato");
  bool thermo = false;
  for (const auto& h : hits)
    if (h.kind == "device" || h.entityId > 0)
      thermo = true;
  check(thermo, "formation: 'el termostato' gazetteered");

  graph.close();
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  return fails == 0 ? 0 : 1;
}

int conflictTest()
{
  const std::string dbPath = "database/conflict-test.db";
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  SqliteGraph graph;
  graph.open(dbPath);
  graph.applySchema();

  const int64_t e1 = graph.createEntity({.kind = "person",
                                         .canonical = "hermana",
                                         .lang = "es",
                                         .personId = std::nullopt});
  const int64_t f1 = graph.upsertFact({.entityId = e1,
                                       .predicate = "visits_on",
                                       .value = "domingos",
                                       .canonical = "visita los domingos",
                                       .type = "schedule",
                                       .priority = 85,
                                       .confidence = 0.9F,
                                       .lang = "es",
                                       .scope = "user",
                                       .refId = kFixtureUser,
                                       .now = 1000,
                                       .sourceId = std::nullopt});
  const int64_t f2 = graph.upsertFact({.entityId = e1,
                                       .predicate = "visits_on",
                                       .value = "sabados",
                                       .canonical = "visita los sabados",
                                       .type = "schedule",
                                       .priority = 90,
                                       .confidence = 0.9F,
                                       .lang = "es",
                                       .scope = "user",
                                       .refId = kFixtureUser,
                                       .now = 2000,
                                       .sourceId = std::nullopt});
  check(f1 > 0 && f2 > 0 && f1 != f2, "conflict: second fact inserted");

  {
    std::scoped_lock lock(graph.mutex());
    SqliteStmt stmt;
    if (stmt.prepare(graph.handle(),
                     "SELECT valid_to FROM memory_fact WHERE id = ?")) {
      stmt.bindInt64(1, f1);
      check(stmt.step() == SQLITE_ROW && stmt.columnInt64(0) == 2000,
            "conflict: old fact closed at supersede time");
    }
    if (stmt.prepare(
            graph.handle(),
            "SELECT count(*) FROM memory_edge WHERE kind = 'supersedes'")) {
      check(stmt.step() == SQLITE_ROW && stmt.columnInt64(0) == 1,
            "conflict: supersedes edge recorded");
    }
  }
  const auto hits = graph.factsForEntity({.entityId = e1,
                                          .scope = "user",
                                          .refId = kFixtureUser,
                                          .maxHops = 1,
                                          .limit = 10});
  bool onlyNew = hits.size() == 1 && hits[0].factId == f2;
  check(onlyNew, "conflict: recall returns only the open fact");

  graph.close();
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  return fails == 0 ? 0 : 1;
}

int graphTest()
{
  const std::string dbPath = "database/graph-test.db";
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  SqliteGraph graph;
  graph.open(dbPath);
  graph.applySchema();
  check(graph.handle() != nullptr, "graph store opened");

  const int64_t now = 1000;
  const int64_t hermana = graph.createEntity({.kind = "person",
                                              .canonical = "hermana",
                                              .lang = "es",
                                              .personId = std::nullopt});
  const int64_t termostato = graph.createEntity({.kind = "device",
                                                 .canonical = "termostato",
                                                 .lang = "es",
                                                 .personId = std::nullopt});
  check(hermana > 0 && termostato > 0 && hermana != termostato,
        "entities created");

  graph.addAlias({.entityId = hermana,
                  .surface = "mi hermana",
                  .norm = "mi hermana",
                  .lang = "es",
                  .personFrame = "first",
                  .confidence = 0.95F});
  graph.addAlias({.entityId = hermana,
                  .surface = "tu hermana",
                  .norm = "tu hermana",
                  .lang = "es",
                  .personFrame = "second",
                  .confidence = 0.9F});
  const auto resolved = graph.resolveEntity(
      {.surface = "mi hermana", .norm = "mi hermana", .lang = "es"});
  check(resolved && *resolved == hermana, "alias exact norm resolves");
  const auto resolvedFts = graph.resolveEntity(
      {.surface = "la hermana", .norm = "la hermana", .lang = "es"});
  check(resolvedFts && *resolvedFts == hermana, "alias fts fallback resolves");
  const auto aliases = graph.aliasesForEntity(hermana);
  check(aliases.size() == 2, "aliases listed for entity");

  const int64_t f1 = graph.upsertFact({.entityId = hermana,
                                       .predicate = "visits_on",
                                       .value = "domingos",
                                       .canonical = "visita los domingos",
                                       .type = "schedule",
                                       .priority = 85,
                                       .confidence = 0.9F,
                                       .lang = "es",
                                       .scope = "user",
                                       .refId = kFixtureUser,
                                       .now = now,
                                       .sourceId = std::nullopt});
  check(f1 > 0, "fact inserted");
  const int64_t f2 = graph.upsertFact({.entityId = hermana,
                                       .predicate = "visits_on",
                                       .value = "sabados",
                                       .canonical = "visita los sabados",
                                       .type = "schedule",
                                       .priority = 90,
                                       .confidence = 0.9F,
                                       .lang = "es",
                                       .scope = "user",
                                       .refId = kFixtureUser,
                                       .now = now + 100,
                                       .sourceId = std::nullopt});
  check(f2 > 0 && f2 != f1, "conflict inserts a new fact");
  check(graph.closeFact(f2, now + 200), "closeFact closes open fact");

  const auto hits = graph.factsForEntity({.entityId = hermana,
                                          .scope = "user",
                                          .refId = kFixtureUser,
                                          .maxHops = 1,
                                          .limit = 10});
  check(hits.empty(), "all facts closed -> no recall hits");

  const int64_t f3 = graph.upsertFact({.entityId = hermana,
                                       .predicate = "likes",
                                       .value = "el cafe",
                                       .canonical = "le gusta el cafe",
                                       .type = "preference",
                                       .priority = 80,
                                       .confidence = 0.9F,
                                       .lang = "es",
                                       .scope = "user",
                                       .refId = kFixtureUser,
                                       .now = now,
                                       .sourceId = std::nullopt});
  const auto hits2 = graph.factsForEntity({.entityId = hermana,
                                           .scope = "user",
                                           .refId = kFixtureUser,
                                           .maxHops = 1,
                                           .limit = 10});
  check(hits2.size() == 1 && hits2[0].factId == f3 && hits2[0].hops == 0,
        "open fact recalled at hop 0");

  graph.upsertFact({.entityId = termostato,
                    .predicate = "is_set_to",
                    .value = "21 grados",
                    .canonical = "esta a 21 grados",
                    .type = "attribute",
                    .priority = 60,
                    .confidence = 0.9F,
                    .lang = "es",
                    .scope = "user",
                    .refId = kFixtureUser,
                    .now = now,
                    .sourceId = std::nullopt});
  graph.addAlias({.entityId = termostato,
                  .surface = "el termostato",
                  .norm = "el termostato",
                  .lang = "es",
                  .personFrame = "none",
                  .confidence = 0.95F});
  {
    std::scoped_lock lock(graph.mutex());
    SqliteStmt stmt;
    if (stmt.prepare(
            graph.handle(),
            "INSERT INTO memory_edge (kind, src_id, dst_id, predicate, "
            "since, until, ord) VALUES ('related', ?, ?, 'controls', 0, 0, "
            "0)")) {
      stmt.bindInt64(1, hermana);
      stmt.bindInt64(2, termostato);
      stmt.step();
    }
  }
  const auto expanded = graph.factsForEntity({.entityId = hermana,
                                              .scope = "user",
                                              .refId = kFixtureUser,
                                              .maxHops = 2,
                                              .limit = 10});
  bool foundRemote = false;
  for (const auto& hit : expanded)
    if (hit.entityId == termostato && hit.hops == 1)
      foundRemote = true;
  check(foundRemote, "related hop-1 expansion recalls remote fact");

  const int64_t ep =
      graph.recordEpisode({.kind = "conversation",
                           .summary = "la hermana viene el domingo",
                           .actor = "",
                           .occurredAt = now,
                           .sessionId = "s1",
                           .lang = "es",
                           .scope = "user",
                           .refId = kFixtureUser,
                           .salience = 0.8F,
                           .sourceId = std::nullopt,
                           .mentionEntityIds = {hermana}});
  check(ep > 0, "episode recorded");
  const auto eps =
      graph.episodesBetween("user", kFixtureUser, now - 1, now + 1, 10);
  check(eps.size() == 1 && eps[0] == ep, "episode found in range");

  {
    std::scoped_lock lock(graph.mutex());
    SqliteStmt stmt;
    if (stmt.prepare(
            graph.handle(),
            "CREATE TABLE IF NOT EXISTS memory_l1 ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  scope TEXT NOT NULL, ref_id INTEGER NOT NULL DEFAULT 0,"
            "  type TEXT NOT NULL, source TEXT,"
            "  content TEXT NOT NULL, priority INTEGER NOT NULL DEFAULT 50,"
            "  hit_count INTEGER NOT NULL DEFAULT 0,"
            "  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,"
            "  deleted_at INTEGER)"))
      stmt.step();
    if (stmt.prepare(
            graph.handle(),
            "INSERT INTO memory_l1 (scope, type, source, content, priority, "
            "hit_count, created_at, updated_at) "
            "VALUES ('user', 'persona', 'rule', 'mi hermana viene los "
            "domingos', 85, 3, 1000, 1000)"))
      stmt.step();
  }
  graph.migrateLegacy();
  graph.migrateLegacy(); // idempotence: second pass must not duplicate
  {
    std::scoped_lock lock(graph.mutex());
    SqliteStmt stmt;
    if (stmt.prepare(
            graph.handle(),
            "SELECT count(*) FROM memory_fact WHERE predicate = 'legacy'")) {
      check(stmt.step() == SQLITE_ROW && stmt.columnInt64(0) == 1,
            "legacy migration imported exactly one fact");
    }
    if (stmt.prepare(
            graph.handle(),
            "SELECT hit_count FROM memory_fact WHERE predicate = 'legacy'")) {
      check(stmt.step() == SQLITE_ROW && stmt.columnInt64(0) == 3,
            "legacy migration preserved hit_count");
    }
  }

  graph.close();
  std::filesystem::remove(dbPath);
  std::filesystem::remove(dbPath + "-wal");
  std::filesystem::remove(dbPath + "-shm");
  return fails == 0 ? 0 : 1;
}

void usage()
{
  std::cout << "argus-memory-probe --schema-check | --capture-test | "
               "--tool-parse-test | --embed-check | --recall-bench | "
               "--vec-gate-test | --dedup-bench | --sim \"<a>\" \"<b>\" | "
               "--purge <scope> <refId> | --seed <userId> \"<memory>\" | "
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
  if (mode == "--capture-query" && argc > 2)
    return captureQuery(argv[2]);
  if (mode == "--tools-test")
    return toolsTest();
  if (mode == "--graph-recall-test")
    return graphRecallTest();
  if (mode == "--entity-test")
    return entityTest();
  if (mode == "--formation-test")
    return formationTest();
  if (mode == "--conflict-test")
    return conflictTest();
  if (mode == "--graph-test")
    return graphTest();
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
  if (mode == "--vec-gate-test")
    return vecGateTest();
  if (mode == "--dedup-bench")
    return dedupBench();
  if (mode == "--sim" && argc >= 4)
    return simPair(argv[2], argv[3]);
  if (mode == "--scale-test" && argc >= 3)
    return scaleTest(std::atoi(argv[2]));
  if (mode == "--vec-rows" && argc >= 3)
    return vecRows(argv[2]);
  if (mode == "--recall-user" && argc >= 4)
    return recallUser(std::strtoll(argv[2], nullptr, 10), argv[3]);
  if (mode == "--sim-scan" && argc >= 3)
    return simScan(argv[2]);
  if (mode == "--sims")
    return simsCheck();
  if (mode == "--purge" && argc >= 4)
    return purgeCmd(argv[2], std::strtoll(argv[3], nullptr, 10));
  if (mode == "--seed" && argc >= 4)
    return seedCmd(std::strtoll(argv[2], nullptr, 10), argv[3]);
  if (mode == "--recall-demo" && argc >= 3)
    return recallDemo(argv[2]);
  if (mode == "--fts-debug" && argc >= 3)
    return ftsDebug(argv[2]);
  if (mode == "--tokens" && argc >= 3)
    return tokenDebug(argv[2]);

  usage();
  return 1;
}
