// labs/kuzu-probe — COGNITIVE_MEMORY_PLAN.md Phase 0 gate (final form).
//
// Exercises the REAL access pattern through KuzuDb (single Database + single
// Connection behind a non-recursive mutex, exactly like VecDb):
//
//   - boot: KuzuDb::init + KuzuDb::applySchema (§4 schema, idempotent)
//   - 5 000 facts with 384-d embeddings + vector/FTS indexes
//   - p50/p95 for HNSW KNN, BM25 FTS (stemmer 'spanish') and a 2-hop
//     traversal on the unloaded store
//   - "recall under consolidation load": the worker thread simulates the
//     consolidation pipeline — e5-small embedding forward (~15 ms) OUTSIDE
//     the lock, then a short Cypher insert INSIDE the lock — while the main
//     thread runs recall queries (KNN/FTS/traversal mix) through the same
//     serialized connection. Gate: p95 recall < 30 ms (Wave A baseline
//     27.3 ms on the SQLite store must not regress).
//   - reopen from disk + RSS delta at open + statically linked binary size
//
// The fts/vector extensions are statically linked and auto-loaded at
// Database construction — if that stopped being true, every CALL below would
// fail with "extension not loaded" and the gate fails closed.

#include "kuzu-db.hxx"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <main/connection.h>
#include <main/query_result.h>
#include <memory>
#include <processor/result/flat_tuple.h>
#include <random>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{

constexpr int kDim = 384;
constexpr int kNumEntities = 200;
constexpr int kNumFacts = 5000;
constexpr int kNumRelated = 600;
constexpr int kNumAliases = 300;
constexpr int kNumSupersedes = 50;
constexpr int kBenchRuns = 100;
// e5-small ONNX forward measured in CONTEXT.md (p50 ~10-25 ms): the
// consolidation worker embeds OUTSIDE the lock, inserts INSIDE it.
constexpr int kEmbedSimMs = 15;
constexpr int kWorkerInserts = 2000;

const std::vector<std::string> kCanonical = {
    "mi hermana viene los domingos a comer",
    "el termostato sube la temperatura por la noche",
    "a mi abuela le gusta el cafe sin azucar",
    "la puerta del garaje se queda abierta",
    "el tecnico revisa la camara el lunes",
    "el wifi no llega al patio trasero",
    "la alarma se activa a las diez de la noche",
    "el perro duerme en el sofa de la sala",
    "los ninos tienen clase de natacion el miercoles",
    "la bomba de agua hace ruido cuando llueve",
};

long long nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

double pct(const std::vector<double>& v, double p)
{
  std::vector<double> s = v;
  std::sort(s.begin(), s.end());
  const size_t idx = static_cast<size_t>(
                         std::ceil(p / 100.0 * static_cast<double>(s.size()))) -
                     1;
  return s[idx];
}

long rssKb()
{
  struct rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0)
    return -1;
  return ru.ru_maxrss;
}

size_t binarySizeBytes()
{
  struct stat st{};
  if (stat("/proc/self/exe", &st) != 0)
    return 0;
  return static_cast<size_t>(st.st_size);
}

std::string embedLiteral(const std::vector<float>& v)
{
  std::string out = "[";
  char buf[32];
  for (size_t i = 0; i < v.size(); ++i) {
    if (i > 0)
      out += ",";
    snprintf(buf, sizeof(buf), "%.6f", v[i]);
    out += buf;
  }
  out += "]";
  return out;
}

// Runs a statement on the single serialized connection. The caller holds
// KuzuDb::mutex().
std::string runLocked(const std::string& sql)
{
  auto result = KuzuDb::handle()->query(sql);
  if (!result->isSuccess()) {
    std::cerr << "query failed: " << sql << "\n"
              << result->getErrorMessage() << "\n";
    std::exit(1);
  }
  std::string out;
  while (result->hasNext()) {
    const auto tuple = result->getNext();
    const size_t cols = tuple->len();
    for (size_t i = 0; i < cols; ++i) {
      if (i > 0)
        out += "|";
      out += tuple->getValue(i)->toString();
    }
    out += "\n";
  }
  return out;
}

double bench(const std::string& sql, int runs, double& p50, double& p95)
{
  std::vector<double> latencies;
  latencies.reserve(static_cast<size_t>(runs));
  for (int i = 0; i < runs; ++i) {
    const long long t0 = nowMs();
    {
      std::scoped_lock lock(KuzuDb::mutex());
      runLocked(sql);
    }
    latencies.push_back(static_cast<double>(nowMs() - t0));
  }
  p50 = pct(latencies, 50.0);
  p95 = pct(latencies, 95.0);
  double sum = 0.0;
  for (double v : latencies)
    sum += v;
  return sum / static_cast<double>(latencies.size());
}

std::vector<float> clusterEmbedding(std::mt19937& rng, int cluster)
{
  static const std::vector<std::vector<float>> centers = [] {
    std::mt19937 seed(7);
    std::uniform_real_distribution<float> d(-0.6F, 0.6F);
    std::vector<std::vector<float>> cs;
    for (int c = 0; c < 10; ++c) {
      std::vector<float> v;
      v.reserve(kDim);
      for (int i = 0; i < kDim; ++i)
        v.push_back(d(seed));
      cs.push_back(v);
    }
    return cs;
  }();
  std::normal_distribution<float> noise(0.0F, 0.05F);
  std::vector<float> v;
  v.reserve(kDim);
  for (int i = 0; i < kDim; ++i) {
    float x = centers[static_cast<size_t>(cluster)][static_cast<size_t>(i)] +
              noise(rng);
    v.push_back(std::clamp(x, -1.0F, 1.0F));
  }
  return v;
}

void insertData(int numFacts)
{
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> entityDist(1, kNumEntities);
  std::uniform_int_distribution<int> predDist(0, 5);

  const std::vector<std::string> kPreds = {"visits_on",  "likes",
                                           "runs_at",    "is_set_to",
                                           "belongs_to", "happens_on"};

  {
    std::scoped_lock lock(KuzuDb::mutex());
    for (int e = 1; e <= kNumEntities; ++e) {
      const char* kinds[] = {"person", "place", "device", "thing"};
      const std::string kind = kinds[e % 4];
      runLocked("CREATE (:Entity {id: " + std::to_string(e) + ", kind: '" +
                kind + "', canonical: 'entity_" + std::to_string(e) +
                "', lang: 'es', created_at: 1, "
                "updated_at: 1})");
    }
  }

  for (int i = 1; i <= numFacts; ++i) {
    const std::string canonical =
        kCanonical[static_cast<size_t>(i) % kCanonical.size()];
    const std::string pred = kPreds[static_cast<size_t>(predDist(rng))];
    const std::vector<float> emb = clusterEmbedding(rng, i % 10);
    const int priority = 50 + (i % 50);
    {
      std::scoped_lock lock(KuzuDb::mutex());
      runLocked("CREATE (:Fact {predicate: '" + pred + "', value: 'value_" +
                std::to_string(i) + "', canonical: '" + canonical +
                "', type: 'persona', priority: " + std::to_string(priority) +
                ", confidence: 0.9, lang: 'es', valid_from: 1000, "
                "valid_to: 0, hit_count: " +
                std::to_string(i % 7) +
                ", created_at: " + std::to_string(1000 + i) +
                ", updated_at: " + std::to_string(1000 + i) +
                ", embedding: " + embedLiteral(emb) + "})");
      runLocked("MATCH (f:Fact {id: " + std::to_string(i) +
                "}), (e:Entity {id: " + std::to_string(entityDist(rng)) +
                "}) CREATE (f)-[:ABOUT {role: 'subject'}]->(e)");
    }
    if (i % 500 == 0) {
      std::cout << "  inserted " << i << "/" << numFacts << " facts\n"
                << std::flush;
    }
  }

  std::scoped_lock lock(KuzuDb::mutex());
  for (int i = 0; i < kNumRelated; ++i) {
    const int a = 1 + (i * 7) % kNumEntities;
    const int b = 1 + (i * 13) % kNumEntities;
    runLocked("MATCH (a:Entity {id: " + std::to_string(a) +
              "}), (b:Entity {id: " + std::to_string(b) +
              "}) CREATE (a)-[:RELATED {predicate: 'knows', since: 100, "
              "until: 0}]->(b)");
  }
  for (int i = 1; i <= kNumAliases; ++i) {
    const int entityId = 1 + (i * 11) % kNumEntities;
    runLocked("CREATE (:Alias {surface: 'alias_" + std::to_string(i) +
              "', norm: 'alias_" + std::to_string(i) +
              "', lang: 'es', person_frame: 'none', confidence: 0.9})");
    runLocked("MATCH (a:Alias {id: " + std::to_string(i) +
              "}), (e:Entity {id: " + std::to_string(entityId) +
              "}) CREATE (a)-[:ALIAS_OF]->(e)");
  }
  for (int i = 0; i < kNumSupersedes; ++i) {
    const int older = 1 + (i * 17) % numFacts;
    const int newer = 1 + (i * 23) % numFacts;
    if (older == newer)
      continue;
    runLocked("MATCH (a:Fact {id: " + std::to_string(older) +
              "}), (b:Fact {id: " + std::to_string(newer) +
              "}) CREATE (a)-[:SUPERSEDES {at: 2000}]->(b)");
  }
}

void createIndexes()
{
  std::scoped_lock lock(KuzuDb::mutex());
  runLocked("CALL CREATE_VECTOR_INDEX('Fact', 'fact_vec', 'embedding', "
            "metric := 'cosine')");
  runLocked("CALL CREATE_FTS_INDEX('Fact', 'fact_fts', ['canonical'], "
            "stemmer := 'spanish')");
  runLocked("CALL CREATE_FTS_INDEX('Alias', 'alias_fts', ['norm'])");
  runLocked("CALL CREATE_FTS_INDEX('Episode', 'ep_fts', ['summary'], "
            "stemmer := 'spanish')");
}

// The consolidation worker: embeds OUTSIDE the lock (the e5-small forward),
// then takes a short write transaction INSIDE it — the §5.1 shape. Batched
// mode holds BEGIN..COMMIT under a SINGLE lock acquisition: a transaction
// must never stay open across a lock release on the shared connection.
void workerLoop(const std::string& insertSql, std::atomic<bool>& stop,
                std::atomic<int>& written, int writerBatch)
{
  std::mt19937 wr(99);
  const bool batched = writerBatch > 1;
  while (!stop.load() && written.load() < kWorkerInserts) {
    const std::vector<float> emb = clusterEmbedding(wr, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(kEmbedSimMs));
    std::scoped_lock lock(KuzuDb::mutex());
    if (batched)
      runLocked("BEGIN TRANSACTION");
    for (int k = 0; k < writerBatch && written.load() < kWorkerInserts; ++k) {
      std::string sql = insertSql;
      const size_t pos = sql.find("$i");
      if (pos != std::string::npos)
        sql.replace(pos, 2, std::to_string(kNumFacts + written.load() + 1));
      runLocked(sql);
      ++written;
    }
    if (batched)
      runLocked("COMMIT");
  }
}

int runGate(const std::string& dbPath, int numFacts, bool withLoad,
            bool seqLoad, const std::string& seqQuery, int writerBatch,
            int turnMs)
{
  std::filesystem::remove_all(dbPath);
  std::filesystem::remove(dbPath + ".wal");
  std::filesystem::remove(dbPath + ".lock");
  std::cout << "db: " << dbPath << "\n" << std::flush;
  std::cout << "binary size (kuzu statically linked): "
            << binarySizeBytes() / (1024 * 1024) << " MB\n"
            << std::flush;

  const long rssBefore = rssKb();
  KuzuDb::init(dbPath);
  const long rssAfterOpen = rssKb();
  std::cout << "rss delta at open: " << (rssAfterOpen - rssBefore) / 1024
            << " MB\n"
            << std::flush;

  KuzuDb::applySchema();
  KuzuDb::applySchema(); // idempotence: second pass must be a no-op
  std::cout << "[ok] schema applied twice (idempotent)\n" << std::flush;

  const long long t0 = nowMs();
  insertData(numFacts);
  std::cout << "[ok] inserted " << numFacts << " facts, " << kNumEntities
            << " entities, " << kNumRelated << " related, " << kNumAliases
            << " aliases in " << (nowMs() - t0) << " ms\n"
            << std::flush;

  createIndexes();
  std::cout << "[ok] vector + fts indexes created (extensions auto-loaded)\n";

  std::mt19937 probeRng(1);
  const std::vector<float> probeVec = clusterEmbedding(probeRng, 3);
  const std::string knnSql = "CALL QUERY_VECTOR_INDEX('Fact', 'fact_vec', " +
                             embedLiteral(probeVec) +
                             ", 10) "
                             "RETURN node, distance";
  const std::string ftsSql =
      "CALL QUERY_FTS_INDEX('Fact', 'fact_fts', 'hermana') RETURN *";
  const std::string travSql =
      "MATCH (a:Entity)-[:RELATED]->(b:Entity)-[:RELATED]->(c:Entity) "
      "WHERE a.id = 5 RETURN count(*)";

  double p50 = 0, p95 = 0;
  const double ftsAvg = bench(ftsSql, kBenchRuns, p50, p95);
  std::cout << "fts bm25 (idle):  p50=" << p50 << " ms p95=" << p95
            << " ms avg=" << ftsAvg << " ms\n"
            << std::flush;
  const double knnAvg = bench(knnSql, kBenchRuns, p50, p95);
  std::cout << "knn hnsw (idle):  p50=" << p50 << " ms p95=" << p95
            << " ms avg=" << knnAvg << " ms\n"
            << std::flush;
  const double travAvg = bench(travSql, kBenchRuns, p50, p95);
  std::cout << "traversal (idle): p50=" << p50 << " ms p95=" << p95
            << " ms avg=" << travAvg << " ms\n"
            << std::flush;

  // Recall under consolidation load: the worker embeds (15 ms, outside the
  // lock) and inserts (inside the lock) while recall runs a realistic query
  // mix through the same serialized connection. Gate: p95 < 30 ms.
  if (withLoad && seqLoad) {
    std::cout << "[phase] sequential load (single thread): insert + "
              << "query mix interleaved\n"
              << std::flush;
    std::vector<double> samples;
    std::vector<std::string> queries;
    if (seqQuery == "knn" || seqQuery == "mix")
      queries.push_back(knnSql);
    if (seqQuery == "fts" || seqQuery == "mix")
      queries.push_back(ftsSql);
    if (seqQuery == "trav" || seqQuery == "mix")
      queries.push_back(travSql);
    if (queries.empty())
      queries.push_back("MATCH (f:Fact) RETURN count(*)");
    std::cout << "  seq query set: " << seqQuery << "\n" << std::flush;
    std::mt19937 wr(99);
    for (int r = 0; r < 50; ++r) {
      const std::vector<float> emb = clusterEmbedding(wr, 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(kEmbedSimMs));
      {
        const long long i0 = nowMs();
        std::scoped_lock lock(KuzuDb::mutex());
        runLocked("CREATE (:Fact {predicate: 'write', value: 'seq_" +
                  std::to_string(r) +
                  "', canonical: 'la alarma suena por la manana', "
                  "type: 'persona', priority: 50, confidence: 0.9, "
                  "lang: 'es', valid_from: 1000, valid_to: 0, "
                  "hit_count: 0, created_at: 1, updated_at: 1, "
                  "embedding: " +
                  embedLiteral(emb) + "})");
        if (r < 5)
          std::cout << "  insert #" << r << " took " << (nowMs() - i0)
                    << " ms\n"
                    << std::flush;
      }
      for (const auto& sql : queries) {
        const long long t0 = nowMs();
        {
          std::scoped_lock lock(KuzuDb::mutex());
          runLocked(sql);
        }
        samples.push_back(static_cast<double>(nowMs() - t0));
      }
    }
    std::cout << "seq-load recall: p50=" << pct(samples, 50.0)
              << " ms p95=" << pct(samples, 95.0) << " ms\n"
              << std::flush;
  }
  else if (withLoad) {
    // Realistic duty cycle: Argus runs ONE recall per conversation turn and
    // a turn is STT + recall + LLM generation + TTS (~2-5 s). The reader
    // therefore idles for turnMs between turns (~0.3-0.8% duty), it does NOT
    // hammer the lock at 100% — the original harness did, starving the
    // worker on a non-FIFO std::mutex (that was the harness's defect, not
    // the engine's; see COGNITIVE_MEMORY_PLAN.md §0).
    std::cout << "[phase] recall under consolidation load (worker batch="
              << writerBatch << ", embed=" << kEmbedSimMs
              << " ms sim, turn=" << turnMs << " ms idle between turns)\n"
              << std::flush;
    std::atomic<bool> stop{false};
    std::atomic<int> written{0};
    const std::string insertSql =
        "CREATE (:Fact {predicate: 'write', value: 'value_$i', "
        "canonical: 'la alarma suena por la manana', type: 'persona', "
        "priority: 50, confidence: 0.9, lang: 'es', valid_from: 1000, "
        "valid_to: 0, hit_count: 0, created_at: 1, updated_at: 1, "
        "embedding: " +
        embedLiteral(clusterEmbedding(probeRng, 1)) + "})";
    std::thread worker(workerLoop, insertSql, std::ref(stop), std::ref(written),
                       writerBatch);

    std::vector<double> samples;
    samples.reserve(6000);
    const std::vector<std::string> queries = {knnSql, ftsSql, travSql};
    int turns = 0;
    while (written.load() < kWorkerInserts) {
      for (const auto& sql : queries) {
        const long long t0 = nowMs();
        {
          std::scoped_lock lock(KuzuDb::mutex());
          runLocked(sql);
        }
        samples.push_back(static_cast<double>(nowMs() - t0));
      }
      ++turns;
      if (turns % 20 == 0)
        std::cout << "  turns=" << turns << " written=" << written.load()
                  << "\n"
                  << std::flush;
      std::this_thread::sleep_for(std::chrono::milliseconds(turnMs));
    }
    stop.store(true);
    worker.join();
    std::cout << "recall under load: p50=" << pct(samples, 50.0)
              << " ms p95=" << pct(samples, 95.0)
              << " ms (worker rows=" << written.load() << ", turns=" << turns
              << ", samples=" << samples.size() << ")\n"
              << std::flush;
  }

  // Reopen: persistence + extension re-load from disk.
  KuzuDb::shutdown();
  KuzuDb::init(dbPath);
  {
    std::scoped_lock lock(KuzuDb::mutex());
    runLocked(knnSql);
  }
  std::cout << "[ok] reopened and queried from disk\n" << std::flush;
  KuzuDb::shutdown();

  const long rssAfter = rssKb();
  std::cout << "rss total peak: " << (rssAfter - rssBefore) / 1024 << " MB\n";
  return 0;
}

} // namespace

int main(int argc, char** argv)
{
  std::string dbPath = "database/kuzu-probe.kz";
  int numFacts = kNumFacts;
  bool withLoad = true;
  bool seqLoad = false;
  std::string seqQuery = "mix";
  int writerBatch = 1;
  int turnMs = 2000;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--db" && i + 1 < argc)
      dbPath = argv[++i];
    else if (arg == "--facts" && i + 1 < argc)
      numFacts = std::atoi(argv[++i]);
    else if (arg == "--no-load")
      withLoad = false;
    else if (arg == "--seq-load")
      seqLoad = true;
    else if (arg == "--seq-query" && i + 1 < argc)
      seqQuery = argv[++i];
    else if (arg == "--writer-batch" && i + 1 < argc)
      writerBatch = std::atoi(argv[++i]);
    else if (arg == "--turn-ms" && i + 1 < argc)
      turnMs = std::atoi(argv[++i]);
    else if (arg == "--help") {
      std::cout << "argus-kuzu-probe [--db <path>] [--facts <n>] "
                   "[--no-load] [--seq-load] [--seq-query knn|fts|trav|mix] "
                   "[--writer-batch <n>] [--turn-ms <n>]\n";
      return 0;
    }
  }
  if (numFacts <= 0)
    numFacts = kNumFacts;
  if (turnMs <= 0)
    turnMs = 2000;
  return runGate(dbPath, numFacts, withLoad, seqLoad, seqQuery, writerBatch,
                 turnMs);
}
