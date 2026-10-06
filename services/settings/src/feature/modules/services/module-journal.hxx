#pragma once

#include <feature/modules/infra/module-action-sink.hxx>
#include <feature/modules/repositories/module-audit/module-audit-repository.hxx>
#include <feature/modules/repositories/module-journal/module-journal-repository.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <stop_token>
#include <thread>
#include <mutex>

namespace module_journal
{
[[nodiscard]] UserActionEvent eventOf(const ModuleAuditSchema& entry);
}

struct ModuleJournalInput
{
  drogon::orm::DbClientPtr db;
  ModuleActionSink* sink{nullptr};
  std::chrono::milliseconds pollInterval{2000};
  int batch{50};
};

class ModuleJournal
{
public:
  explicit ModuleJournal(const ModuleJournalInput& input);
  ~ModuleJournal();
  ModuleJournal(const ModuleJournal&) = delete;
  ModuleJournal& operator=(const ModuleJournal&) = delete;
  ModuleJournal(ModuleJournal&&) = delete;
  ModuleJournal& operator=(ModuleJournal&&) = delete;

  int relay();

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  void loop(const std::stop_token& stop);

  ModuleAuditRepository audit_;
  ModuleJournalRepository journal_;
  ModuleActionSink* sink_;
  std::chrono::milliseconds pollInterval_;
  int batch_;

  std::mutex waitMutex_;
  std::condition_variable_any wakeUp_;
  std::atomic<bool> running_{false};
  std::jthread worker_;
};
