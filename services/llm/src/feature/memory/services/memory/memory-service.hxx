#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared/vocabulary/tool-contracts.hxx>
#include <feature/memory/repositories/memory-graph/memory-graph-repository.hxx>
#include <feature/memory/services/embedding/embedding-service.hxx>
#include <feature/memory/services/extract/tiered-extractor.hxx>
#include <feature/memory/services/memory/graph-recall.hxx>
#include <feature/memory/services/memory/memory-chat.hxx>
#include <feature/memory/services/memory/memory-formation.hxx>
#include <feature/memory/services/memory/reminder-call-scheduler.hxx>
#include <phrase/phrase-catalog.hxx>
#include <feature/memory/services/memory/sqlite-graph.hxx>
#include <feature/memory/services/memory/tool-parser.hxx>
#include <sqlite/vec-db.hxx>
#include <string>
#include <thread>
#include <vector>

struct CaptureInput
{
  int64_t userId;
  std::string lang;
  std::string text;
};

struct TranscriptJobInput
{
  int64_t userId;
  std::string transcript;
  std::string lang;
};

struct SystemEventInput
{
  std::string channel;
  std::string summary;
  std::string actor;
  int64_t at;
  int64_t userId;
  std::string lang;
  std::vector<int64_t> entitiesHint;
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

struct MemoryInitOptions
{
  bool deferStore = false;
};

class MemoryService
{
public:
  MemoryService(VecDb& vecDb, IMemoryChat& chat) : vecDb_(vecDb), chat_(chat) {}
  ~MemoryService();

  MemoryService(const MemoryService&) = delete;
  MemoryService& operator=(const MemoryService&) = delete;

  void init(const MemoryInitOptions& options = {});
  void openStore();
  void shutdown();
  bool isLoaded() const;

  void requestStop();
  [[nodiscard]] bool drained() const;

  CaptureResult captureExplicit(const CaptureInput& input);
  CaptureResult captureImplicit(const CaptureInput& input);
  RecallContext recall(const RecallInput& input);
  void bumpHitCount(const std::vector<int64_t>& ids);
  void bumpEpisodeHits(const std::vector<int64_t>& ids);
  int64_t resolveAddresseeEntity(const std::string& lang);
  std::string profileFor(int64_t userId, const std::string& lang);

  std::string durableTranscript(const std::string& transcript,
                                const std::string& lang) const;

  void enqueueSummary(const TranscriptJobInput& input);
  void enqueueCompaction(const TranscriptJobInput& input);
  bool flushPending(int timeoutMs = 0);

  void waitForIdle(int waitMs);

  int64_t observeSystemEvent(const SystemEventInput& input);

  std::vector<tools::ToolDescriptor> toolDescriptors();

  void setReminderCalls(std::shared_ptr<const ReminderCallScheduler> scheduler);

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

  struct NoteRefinement
  {
    const FormationResult& note;
    int64_t userId = 0;
    std::string text;
    std::string lang;
  };

  VecDb& vecDb_;
  IMemoryChat& chat_;
  std::shared_ptr<const ReminderCallScheduler> reminderCalls_;
  std::unique_ptr<SqliteGraph> graph_{std::make_unique<SqliteGraph>()};
  MemoryGraphRepository graphRepo_;
  EntityResolver resolver_{*graph_};
  EmbeddingService embedding_;
  PhraseCatalog phrases_;
  RuleParser ruleParser_{phrases_};
  ExtractionService extractModel_;
  TieredExtractor extractor_{extractModel_};
  MemoryFormation formation_{MemoryFormationDeps{.graph = *graph_,
                                                 .resolver = resolver_,
                                                 .ruleParser = ruleParser_}};
  GraphRecall graphRecall_{GraphRecallDeps{.graph = *graph_,
                                           .resolver = resolver_,
                                           .embedding = embedding_,
                                           .vecDb = vecDb_}};

  std::mutex storeMutex_;
  bool storeOpen_ = false;

  std::thread worker_;
  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::deque<MemoryJob> queue_;
  size_t queueBound_ = 64;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> workerDone_{true};
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
  void processCompact(const MemoryJob& job);
  void processExtract(const MemoryJob& job);
  void processProfile(const MemoryJob& job);
  std::string buildProfile(int64_t userId, const std::string& lang);
  int64_t captureInline(const InlineCapture& capture);
  void deferCapture(const InlineCapture& capture);
  void refineLater(const NoteRefinement& refinement);
  struct EmbedRequest
  {
    int64_t id{0};
    bool episode{false};
    bool dedup{true};
  };

  bool embedAndStore(const EmbedRequest& request);
  void rebuildAll();
  bool vecLayoutOutdated();

  static bool hasUserScope(int64_t userId);

  tools::ToolResult handleRemember(const tools::ToolCall& call);
  tools::ToolResult handleRemind(const tools::ToolCall& call);

  struct ReminderCallInput
  {
    const tools::ToolCall& call;
    const std::string& text;
    int64_t factId{0};
  };

  std::optional<std::string>
  scheduleReminderCall(const ReminderCallInput& input) const;
  tools::ToolResult handleRecall(const tools::ToolCall& call);
  tools::ToolResult handleForget(const tools::ToolCall& call);
};
