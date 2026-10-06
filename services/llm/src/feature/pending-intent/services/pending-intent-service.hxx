#pragma once

#include <feature/memory/services/memory/reminder-row-writer.hxx>
#include <feature/memory/services/memory/sqlite-graph.hxx>
#include <feature/pending-intent/repositories/pending-intent/pending-intent-repository.hxx>
#include <feature/pending-intent/services/intent-notifier.hxx>
#include <shared/vocabulary/intent-ledger.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct ModuleLabel
{
  std::string module;
  std::string lang;
};

struct PendingIntentDependencies
{
  SqliteGraph* graph{nullptr};
  std::function<tools::ToolResult(const tools::ToolCall&, UserRole)> run;
  std::shared_ptr<const IntentNotifier> notifier;
  std::shared_ptr<const ReminderRowWriter> reminders;
  std::function<std::string(const ModuleLabel&)> moduleName;
  std::function<bool(const std::string&)> moduleActive;
  std::function<int64_t()> clock;
};

struct PendingIntentLimits
{
  int64_t offerTtlS{3600};
  int64_t waitTtlS{86400};
  int64_t keepSettledS{int64_t{30} * 86400};
  std::chrono::milliseconds sweepEvery{60000};
};

struct ModuleFailure
{
  std::string module;
  std::string reason;
};

class PendingIntentService final : public tools::IntentLedger
{
public:
  PendingIntentService(PendingIntentDependencies dependencies, PendingIntentLimits limits);
  ~PendingIntentService() override;

  void offered(const tools::IntentOffer& offer) override;
  void accepted(const tools::ModuleAcceptance& acceptance) override;

  void moduleActivated(const std::string& module);
  void moduleFailed(const ModuleFailure& failure);
  void sweep();

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

  void postActivated(std::string module);
  void postFailed(ModuleFailure failure);
  void postSweep();

private:
  struct Event
  {
    enum class Kind : std::uint8_t
    {
      Activated,
      Failed,
      Sweep
    };

    Kind kind{Kind::Sweep};
    std::string module;
    std::string reason;
  };

  void post(Event event);
  void run(const std::stop_token& stop);
  void handle(const Event& event);
  void execute(const PendingIntentRow& row);
  void fail(const PendingIntentRow& row, const std::string& reason);
  [[nodiscard]] bool settle(const PendingIntentSettleInput& input);
  [[nodiscard]] std::vector<PendingIntentRow> waitingFor(const std::string& module);
  [[nodiscard]] int64_t now() const;
  void tell(const PendingIntentRow& row, const IntentNotice& notice) const;

  PendingIntentDependencies dependencies_;
  PendingIntentLimits limits_;
  PendingIntentRepository repository_;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::deque<Event> events_;
  std::atomic<bool> running_{false};
  std::jthread worker_;
};
