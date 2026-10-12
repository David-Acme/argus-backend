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
std::tm localClockNow()
{
  const auto now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  return local;
}

std::string postureFingerprint(const GuardPosture& posture)
{
  std::string text = guardModeToString(posture.mode);
  text += "|";
  text += posture.occupancy;
  text += posture.publicPresent ? "|1" : "|0";
  text += posture.staffOnly ? "|1" : "|0";
  return text;
}

std::string postureKey(int64_t environmentId)
{
  return "posture." + std::to_string(environmentId);
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
  drogon::async_run([this]() -> drogon::Task<void> {
    pending_.fetch_add(1, std::memory_order_acq_rel);
    co_await evaluate();
    pending_.fetch_sub(1, std::memory_order_acq_rel);
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
      const std::string fingerprint = postureFingerprint(posture);
      const std::string key = postureKey(environment.id);
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
               .ids = {environment.id},
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
