#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <shared/repositories/memory-graph/memory-graph-repository.hxx>
#include <shared/services/embedding/embedding-service.hxx>
#include <shared/services/extract/tiered-extractor.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/memory/graph-recall.hxx>
#include <shared/services/memory/memory-formation.hxx>
#include <shared/services/memory/phrase-catalog.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/services/memory/tool-parser.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <shared/services/tools/tool-registry.hxx>
#include <string>
#include <thread>
#include <vector>

struct CaptureInput
{
  int64_t userId;
  std::string lang;
  std::string text;
};

enum class CaptureOutcome
{
  Rejected,
  Deferred,
  Stored,
};

struct CaptureResult
{
  CaptureOutcome outcome = CaptureOutcome::Rejected;
  int64_t factId = 0;
};

// deferStore: open the store from drogon's beginning advice instead of
// init(), because app().run() logs a FATAL from
// sqlite3_config(SQLITE_CONFIG_MULTITHREAD) if sqlite3 is already up.
struct MemoryInitOptions
{
  bool deferStore = false;
};

class MemoryService
{
public:
  MemoryService(VecDb& vecDb, LlmService& llm) : vecDb_(vecDb), llm_(llm) {}
  ~MemoryService();

  MemoryService(const MemoryService&) = delete;
  MemoryService& operator=(const MemoryService&) = delete;

  void init(const MemoryInitOptions& options = {});
  // Opens the graph/vec store and starts the worker. Idempotent.
  void openStore();
  void shutdown();
  bool isLoaded() const;

  CaptureResult captureExplicit(const CaptureInput& input);
  CaptureResult captureImplicit(const CaptureInput& input);
  CaptureResult captureToolCall(int64_t userId, const std::string& lang,
                                const ToolCall& call);
  RecallContext recall(const RecallInput& input);
  void bumpHitCount(const std::vector<int64_t>& ids);
  void bumpEpisodeHits(const std::vector<int64_t>& ids);
  int64_t resolveAddresseeEntity(const std::string& lang);
  std::string profileFor(int64_t userId, const std::string& lang);

  // Drops transient user turns (questions, retractions) and the assistant
  // answers bound to them, so a session of small talk never becomes an
  // episode. Public because the memory probe asserts on it directly.
  std::string durableTranscript(const std::string& transcript,
                                const std::string& lang) const;

  void enqueueSummary(int64_t userId, const std::string& transcript,
                      const std::string& lang);
  void enqueueCompaction(int64_t userId, const std::string& transcript,
                         const std::string& lang);
  // Waits for the worker queue to drain. timeoutMs 0 = memory.flush_timeout_ms.
  // Returns false when it gave up with work still pending.
  bool flushPending(int timeoutMs = 0);

  int64_t observeSystemEvent(const std::string& channel,
                             const std::string& summary,
                             const std::string& actor, int64_t at,
                             int64_t userId, const std::string& lang,
                             const std::vector<int64_t>& entitiesHint);
  int64_t recordProcedure(const std::string& name, const std::string& goal,
                          const std::string& steps);

  void registerTools(ToolRegistry& registry);

  SemanticGraph& graph() { return *graph_; }
  MemoryFormation& formation() { return formation_; }
  GraphRecall& graphRecall() { return graphRecall_; }
  EntityResolver& resolver() { return resolver_; }

private:
  struct MemoryJob
  {
    enum class Kind
    {
      Embed,
      Compact,
      Rebuild,
      Extract,
      Profile,
    };
    Kind kind;
    int64_t memoryId = 0;
    int64_t userId = 0;
    std::string text;
    std::string lang;
    bool preferIdle = false;
    bool salient = false;
    bool episode = false;
  };

  struct InlineCapture
  {
    std::string channel;
    std::string text;
    std::string lang;
    int64_t userId = 0;
    bool salient = false;
    bool preferIdle = false;
  };

  VecDb& vecDb_;
  LlmService& llm_;
  std::unique_ptr<SqliteGraph> graph_{std::make_unique<SqliteGraph>()};
  MemoryGraphRepository graphRepo_;
  EntityResolver resolver_{*graph_};
  EmbeddingService embedding_;
  PhraseCatalog phrases_;
  RuleParser ruleParser_{phrases_};
  ExtractionService extractModel_;
  TieredExtractor extractor_{extractModel_};
  MemoryFormation formation_{*graph_, resolver_, ruleParser_};
  GraphRecall graphRecall_{*graph_, resolver_, embedding_, vecDb_};

  std::mutex storeMutex_;
  bool storeOpen_ = false;

  std::thread worker_;
  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::deque<MemoryJob> queue_;
  bool stop_ = false;
  bool running_ = false;
  std::atomic<bool> working_{false};

  struct ProfileCache
  {
    int64_t userId = 0;
    std::string lang;
    std::string text;
    std::time_t built = 0;
  };
  mutable std::mutex profileMutex_;
  ProfileCache profileCache_;

  void workerLoop();
  void startWorker();
  void stopWorker();
  void enqueueJob(MemoryJob job);
  void processJob(const MemoryJob& job);
  void waitForIdle(int waitMs);
  void processCompact(const MemoryJob& job);
  void processExtract(const MemoryJob& job);
  void processProfile(const MemoryJob& job);
  std::string buildProfile(int64_t userId, const std::string& lang);
  int64_t captureInline(const InlineCapture& capture);
  void deferCapture(const InlineCapture& capture);
  void embedAndStore(int64_t factId, bool episode);
  void rebuildAll();

  tools::ToolResult handleRemember(const tools::ToolCall& call);
  tools::ToolResult handleRecall(const tools::ToolCall& call);
  tools::ToolResult handleForget(const tools::ToolCall& call);
  tools::ToolResult handleProcedureRun(const tools::ToolCall& call);
};
