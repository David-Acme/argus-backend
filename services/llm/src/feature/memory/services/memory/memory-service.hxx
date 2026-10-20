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
#include <feature/memory/services/extract/covered-lexicon-extractor.hxx>
#include <feature/memory/services/extract/repeat-rule.hxx>
#include <feature/memory/services/memory/graph-recall.hxx>
#include <feature/memory/services/memory/memory-chat.hxx>
#include <feature/memory/services/memory/memory-formation.hxx>
#include <feature/memory/services/memory/reminder-call-scheduler.hxx>
#include <feature/memory/services/memory/reminder-row-writer.hxx>
#include <phrase/phrase-catalog.hxx>
#include <feature/memory/services/memory/sqlite-graph.hxx>
#include <feature/memory/services/memory/tool-parser.hxx>
#include <sqlite/vec-db.hxx>
#include <string>
#include <thread>
#include <vector>

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

  RecallContext recall(const RecallInput& input);
  void bumpHitCount(const std::vector<int64_t>& ids);
  void bumpEpisodeHits(const std::vector<int64_t>& ids);
  int64_t resolveAddresseeEntity(const std::string& lang);
  std::string profileFor(int64_t userId, const std::string& lang);

  std::string durableTranscript(const TranscriptJobInput& input) const;

  void enqueueSummary(const TranscriptJobInput& input);
  void enqueueCompaction(const TranscriptJobInput& input);
  bool flushPending(int timeoutMs = 0);

  void waitForIdle(int waitMs);

  int64_t observeSystemEvent(const SystemEventInput& input);

  std::vector<tools::ToolDescriptor> toolDescriptors();

  void setReminderCalls(std::shared_ptr<const ReminderCallScheduler> scheduler);
  void setReminderRows(std::shared_ptr<const ReminderRowWriter> writer);

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
      Profile,
    };
    Kind kind;
    int64_t memoryId = 0;
    int64_t userId = 0;
    std::string text;
    std::string lang;
    bool preferIdle = false;
    bool episode = false;
  };

  VecDb& vecDb_;
  IMemoryChat& chat_;
  std::shared_ptr<const ReminderCallScheduler> reminderCalls_;
  std::shared_ptr<const ReminderRowWriter> reminderRows_;
  std::unique_ptr<SqliteGraph> graph_{std::make_unique<SqliteGraph>()};
  MemoryGraphRepository graphRepo_;
  EntityResolver resolver_{*graph_};
  EmbeddingService embedding_;
  PhraseCatalog phrases_;
  RuleParser ruleParser_{phrases_};
  CoveredLexiconExtractor extractor_;
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
  void processProfile(const MemoryJob& job);
  std::string buildProfile(int64_t userId, const std::string& lang);
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

  struct ReminderScheduled
  {
    int64_t fireAt{0};
    bool attempted{false};
    bool called{false};
    bool listed{false};
    ReminderCallOutcome why{ReminderCallOutcome::Refused};
  };

  std::optional<ReminderScheduled> scheduleReminder(const ReminderCallInput& input) const;

  struct RepeatAsked
  {
    std::optional<Repeat> repeat{};
    bool unreadable{false};
    DayPart part{DayPart::None};
  };

  struct SeriesInput
  {
    const tools::ToolCall& call;
    const std::string& text;
    int64_t factId{0};
    const Repeat& repeat;
    DayPart part{DayPart::None};
  };

  struct SeriesScheduled
  {
    int64_t firstAt{0};
    bool listed{false};
  };

  static RepeatAsked repeatAsked(const tools::ToolCall& call);
  std::optional<SeriesScheduled> scheduleSeries(const SeriesInput& input) const;
  tools::ToolResult handleReminderList(const tools::ToolCall& call);
  tools::ToolResult handleRecall(const tools::ToolCall& call);
  tools::ToolResult handleForget(const tools::ToolCall& call);
};
