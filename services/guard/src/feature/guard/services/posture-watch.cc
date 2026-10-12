#include "posture-watch.hxx"

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/guard/controllers/guard-errors.hxx>
#include <feature/guard/guard-repository.hxx>
#include <feature/guard/guard-schedule.hxx>
#include <feature/guard/infra/resource-change-outbox.hxx>
#include <feature/guard/repositories/environment/environment-repository.hxx>
#include <shared/vocabulary/guard-mode.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <string>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
struct PendingGuard
{
  explicit PendingGuard(std::atomic<int>* pending) : pending(pending) {}

  PendingGuard(const PendingGuard&) = delete;
  PendingGuard& operator=(const PendingGuard&) = delete;
  PendingGuard(PendingGuard&&) = delete;
  PendingGuard& operator=(PendingGuard&&) = delete;
  ~PendingGuard() { pending->fetch_sub(1, std::memory_order_acq_rel); }

  std::atomic<int>* pending;
};

std::tm localClockNow()
{
  const auto now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  return local;
}
}

PostureWatch::PostureWatch(Dependencies dependencies, Config config)
    : dependencies_(dependencies), config_(std::move(config))
{
  if (!config_.clock.now)
    config_.clock.now = []() {
      return static_cast<int64_t>(std::time(nullptr));
    };
  if (!config_.clock.local)
    config_.clock.local = localClockNow;
}

void PostureWatch::start()
{
  if (timer_.has_value())
    return;
  schedule();
  timer_ = drogon::app().getLoop()->runEvery(
      config_.intervalSeconds, [this]() { schedule(); });
}

void PostureWatch::requestStop()
{
  stopped_.store(true, std::memory_order_release);
  if (timer_.has_value() && drogon::app().isRunning())
    drogon::app().getLoop()->invalidateTimer(*timer_);
  timer_.reset();
}

bool PostureWatch::drained() const
{
  return pending_.load(std::memory_order_acquire) == 0;
}

void PostureWatch::schedule()
{
  if (stopped_.load(std::memory_order_acquire))
    return;
  pending_.fetch_add(1, std::memory_order_acq_rel);
  drogon::async_run([this]() -> drogon::Task<void> {
    const PendingGuard guard{&pending_};
    co_await evaluate();
  });
}

drogon::Task<void> PostureWatch::evaluate() const
{
  const EnvironmentRepository environments;
  const GuardRepository repository;
  const int64_t at = config_.clock.now();
  const std::tm local = config_.clock.local();
  try {
    for (const auto& environment : co_await environments.list()) {
      const GuardSchedule schedule =
          guard_schedule::fromEnvironment(environment);
      const GuardPosture posture = guard_schedule::resolve(
          {.schedule = schedule, .manual = environment.mode, .local = local});
      const std::string fingerprint = guard_schedule::fingerprint(posture);
      const std::string key = guard_schedule::stateKey(environment.id);
      if (co_await repository.state(key, "") == fingerprint)
        continue;

      auto transaction = co_await db_transaction::begin(DbService::client());
      try {
        co_await repository.setState(
            {.key = key, .value = fingerprint, .updatedAt = at,
             .client = transaction.get()});
        if (dependencies_.changes != nullptr) {
          co_await dependencies_.changes->record(
              {.scope = ResourceScope::Environments,
               .ids = {environment.id}, .user = 0,
               .client = transaction.get()});
        }
        if (!co_await db_transaction::Commit(std::move(transaction)))
          throw ResponseException(GuardErrors::ChangeNotRecorded);
      }
      catch (...) {
        db_transaction::rollback(transaction);
        throw;
      }
      LOG_INFO << "Guard posture: environment " << environment.id
               << " is now " << guardModeToString(posture.mode);
    }
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard posture: evaluation failed: " << error.what();
  }
  co_return;
}
