#include "audit-retention-service.hxx"

#include <algorithm>
#include <chrono>
#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

namespace
{
constexpr double kWarmupSeconds = 30.0;
constexpr double kSweepIntervalSeconds = 24.0 * 3600.0;
constexpr int64_t kMsPerDay = 86'400'000;
}

AuditRetentionService::~AuditRetentionService()
{
  stop();
}

void AuditRetentionService::start(const int retentionDays)
{
  if (started_) {
    LOG_WARN << "Audit retention: already started; this start("
             << retentionDays << ") is ignored";
    return;
  }
  started_ = true;

  if (retentionDays <= 0) {
    LOG_INFO << "Audit retention: disabled; audit rows are kept indefinitely";
    return;
  }

  retentionDays_ = retentionDays;
  warmupTimer_ = drogon::app().getLoop()->runAfter(kWarmupSeconds, [this]() {
    runSweep();
  });
  dailyTimer_ =
      drogon::app().getLoop()->runEvery(kSweepIntervalSeconds, [this]() {
        runSweep();
      });
  LOG_INFO << "Audit retention: window " << retentionDays_ << " days";
}

void AuditRetentionService::stop()
{
  clearTimer(warmupTimer_);
  clearTimer(dailyTimer_);
}

void AuditRetentionService::requestStop()
{
  stop();
}

bool AuditRetentionService::drained() const
{
  return !running_.load(std::memory_order_acquire);
}

void AuditRetentionService::clearTimer(std::optional<uint64_t>& timer)
{
  if (!timer.has_value())
    return;
  if (drogon::app().isRunning())
    drogon::app().getLoop()->invalidateTimer(*timer);
  timer.reset();
}

void AuditRetentionService::runSweep()
{
  if (running_.exchange(true, std::memory_order_acq_rel)) {
    LOG_WARN << "Audit retention: sweep already running; skipping this tick";
    return;
  }

  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const int64_t cutoffMs =
      now - static_cast<int64_t>(retentionDays_) * kMsPerDay;
  drogon::async_run([this, cutoffMs]() -> drogon::Task<void> {
    try {
      const int64_t removed = co_await sweep(cutoffMs);
      if (removed > 0)
        LOG_INFO << "Audit retention: compacted " << removed
                 << " audit rows older than " << retentionDays_ << " days";
    }
    catch (const std::exception& error) {
      LOG_WARN << "Audit retention: sweep failed: " << error.what();
    }
    catch (...) {
      LOG_WARN << "Audit retention: sweep failed with unknown error";
    }
    running_.store(false, std::memory_order_release);
  });
}

drogon::Task<int64_t> AuditRetentionService::sweep(const int64_t cutoffMs) const
{
  int64_t removed = 0;
  int64_t auditCursor = 0;
  int64_t userCursor = 0;
  bool auditDone = false;
  bool userDone = false;
  while (!auditDone || !userDone) {
    if (!auditDone) {
      const auto round = co_await auditLogService_.compact(
          {.cutoffMs = cutoffMs, .afterId = auditCursor});
      removed += round.removed;
      auditDone = round.removed == 0;
      auditCursor = std::max(auditCursor, round.lastOlderId);
    }
    if (!userDone) {
      const auto round = co_await userAuditLogService_.compact(
          {.cutoffMs = cutoffMs, .afterId = userCursor});
      removed += round.removed;
      userDone = round.removed == 0;
      userCursor = std::max(userCursor, round.lastOlderId);
    }
  }
  co_return removed;
}
