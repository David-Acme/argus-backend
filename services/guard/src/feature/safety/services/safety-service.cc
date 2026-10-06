#include "safety-service.hxx"

#include <feature/safety/safety-errors.hxx>
#include <feature/safety/services/pin-hash.hxx>

#include <errors/response-exception.hxx>
#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <drogon/drogon.h>
#include <map>
#include <mutex>
#include <unordered_set>
#include <utility>

struct SafetyRetry
{
  int failures{0};
  int64_t nextAt{0};
};

struct SafetyRuntime
{
  std::atomic<bool> alive{true};
  std::atomic<int> active{0};
  std::atomic<bool> sweeping{false};
  std::mutex mutex;
  std::map<int64_t, SafetyRetry> retries;
  std::unordered_set<int64_t> delivering;
  int64_t lastPurgeAt{0};
  std::optional<uint64_t> timer;
};

namespace
{
constexpr int64_t kPurgeIntervalS = 3600;

int64_t systemNow()
{
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct PinMatch
{
  bool disarm{false};
  bool duress{false};
};

class RuntimeScope
{
public:
  explicit RuntimeScope(std::shared_ptr<SafetyRuntime> runtime) : runtime_(std::move(runtime)) {}
  ~RuntimeScope() { runtime_->active.fetch_sub(1, std::memory_order_acq_rel); }
  RuntimeScope(const RuntimeScope&) = delete;
  RuntimeScope& operator=(const RuntimeScope&) = delete;

private:
  std::shared_ptr<SafetyRuntime> runtime_;
};

class PinAttemptScope
{
public:
  PinAttemptScope(std::shared_ptr<PinAttempts> attempts, int64_t userId)
      : attempts_(std::move(attempts)), userId_(userId)
  {
  }
  ~PinAttemptScope() { attempts_->settle({.userId = userId_, .success = success_}); }
  PinAttemptScope(const PinAttemptScope&) = delete;
  PinAttemptScope& operator=(const PinAttemptScope&) = delete;

  void succeed() { success_ = true; }

private:
  std::shared_ptr<PinAttempts> attempts_;
  int64_t userId_{0};
  bool success_{false};
};
}

SafetyService::SafetyService(Dependencies dependencies, Config config)
    : dependencies_(std::move(dependencies)),
      config_(config),
      attempts_(std::make_shared<PinAttempts>(config.attempts)),
      runtime_(std::make_shared<SafetyRuntime>())
{
  if (!dependencies_.clock)
    dependencies_.clock = systemNow;
}

SafetyService::~SafetyService()
{
  requestStop();
}

int64_t SafetyService::now() const
{
  return dependencies_.clock();
}

void SafetyService::start()
{
  if (runtime_->timer || !runtime_->alive.load(std::memory_order_acquire))
    return;
  const auto sweep = [this, runtime = runtime_]() {
    if (!runtime->alive.load(std::memory_order_acquire) ||
        runtime->sweeping.exchange(true, std::memory_order_acq_rel))
      return;
    runtime->active.fetch_add(1, std::memory_order_acq_rel);
    drogon::async_run([this, runtime]() -> drogon::Task<void> {
      const RuntimeScope scope(runtime);
      try {
        if (const size_t delivered = co_await sweepPending(); delivered > 0)
          LOG_WARN << "Guard safety: delivered " << delivered << " pending alert(s)";
        co_await purgeExpired();
      }
      catch (const std::exception& error) {
        LOG_WARN << "Guard safety: sweep failed: " << error.what();
      }
      runtime->sweeping.store(false, std::memory_order_release);
    });
  };
  runtime_->timer = drogon::app().getLoop()->runEvery(config_.sweepIntervalS, sweep);
  drogon::app().getLoop()->queueInLoop(sweep);
}

void SafetyService::requestStop()
{
  runtime_->alive.store(false, std::memory_order_release);
  if (runtime_->timer && drogon::app().isRunning())
    drogon::app().getLoop()->invalidateTimer(*runtime_->timer);
  runtime_->timer.reset();
}

bool SafetyService::drained() const
{
  return runtime_->active.load(std::memory_order_acquire) == 0;
}

drogon::Task<SafetyStatus> SafetyService::status(int64_t userId) const
{
  const SafetySetting setting = co_await settingRepository_.find();
  const auto pin = co_await pinRepository_.find(userId);
  co_return SafetyStatus{.duressEnabled = setting.duressEnabled,
                         .hasPin = setting.duressEnabled && pin.has_value()};
}

drogon::Task<DisarmVerdict> SafetyService::verifyPin(const PinCheck& check) const
{
  const DisarmRequest& request = check.request;
  if (!request.pin || request.pin->empty())
    throw ResponseException(SafetyErrors::PinRequired);
  const PinReservation reservation = attempts_->reserve({.userId = request.userId, .now = now()});
  if (!reservation.admitted)
    throw ResponseException(SafetyErrors::PinLocked);
  PinAttemptScope attempt(attempts_, request.userId);
  const PinMatch match = co_await BlockingTask<PinMatch>{
      [entered = *request.pin, disarm = check.pin.disarmHash, duress = check.pin.duressHash]() {
        const bool disarmMatch = pin_hash::verify({.pin = entered, .stored = disarm});
        const bool duressMatch = pin_hash::verify({.pin = entered, .stored = duress});
        return PinMatch{.disarm = disarmMatch, .duress = duressMatch};
      },
      BlockingLane::Heavy};
  if (reservation.locked) {
    if (match.duress)
      duress(request);
    throw ResponseException(SafetyErrors::PinLocked);
  }
  if (match.duress) {
    attempt.succeed();
    co_return DisarmVerdict::Duress;
  }
  if (match.disarm) {
    attempt.succeed();
    co_return DisarmVerdict::Allowed;
  }
  throw ResponseException(SafetyErrors::PinInvalid);
}

drogon::Task<void> SafetyService::requireCurrentPin(const DisarmRequest& request) const
{
  const auto stored = co_await pinRepository_.find(request.userId);
  if (!stored)
    co_return;
  if (co_await verifyPin({.request = request, .pin = *stored}) == DisarmVerdict::Duress)
    duress(request);
}

drogon::Task<SafetyStatus> SafetyService::setPin(const PinSetInput& input) const
{
  const SafetySetting setting = co_await settingRepository_.find();
  if (!setting.duressEnabled)
    throw ResponseException(SafetyErrors::DuressDisabled);
  if (input.disarmPin == input.duressPin)
    throw ResponseException(SafetyErrors::PinsMustDiffer);
  const DisarmRequest request{.userId = input.userId,
                              .userName = input.userName,
                              .pin = input.currentPin,
                              .environmentId = std::nullopt};
  co_await requireCurrentPin(request);
  const int iterations = config_.pinIterations;
  const auto hashes = co_await BlockingTask<std::pair<std::string, std::string>>{
      [disarm = input.disarmPin, duress = input.duressPin, iterations]() {
        return std::pair{pin_hash::make({.pin = disarm, .iterations = iterations}),
                         pin_hash::make({.pin = duress, .iterations = iterations})};
      },
      BlockingLane::Heavy};
  co_await pinRepository_.upsert({.userId = input.userId,
                                  .disarmHash = hashes.first,
                                  .duressHash = hashes.second,
                                  .now = now()});
  attempts_->clear(input.userId);
  co_return SafetyStatus{.duressEnabled = true, .hasPin = true};
}

drogon::Task<SafetyStatus> SafetyService::removePin(const PinRemoveInput& input) const
{
  const DisarmRequest request{.userId = input.userId,
                              .userName = input.userName,
                              .pin = input.currentPin,
                              .environmentId = std::nullopt};
  co_await requireCurrentPin(request);
  co_await pinRepository_.remove(input.userId);
  attempts_->clear(input.userId);
  const SafetySetting setting = co_await settingRepository_.find();
  co_return SafetyStatus{.duressEnabled = setting.duressEnabled, .hasPin = false};
}

drogon::Task<void> SafetyService::forgetUser(int64_t userId) const
{
  co_await pinRepository_.remove(userId);
  attempts_->clear(userId);
}

drogon::Task<SafetySetting> SafetyService::setting() const
{
  co_return co_await settingRepository_.find();
}

drogon::Task<SafetySetting> SafetyService::toggle(const SafetyToggleInput& input) const
{
  if (!input.duressEnabled && (co_await settingRepository_.find()).duressEnabled)
    co_await requireCurrentPin({.userId = input.actorUserId,
                                .userName = input.actorName,
                                .pin = input.currentPin,
                                .environmentId = std::nullopt});
  const int64_t at = now();
  co_await settingRepository_.update(
      {.duressEnabled = input.duressEnabled, .updatedBy = input.actorUserId, .now = at});
  if (!input.duressEnabled)
    co_await pinRepository_.removeAll();
  co_return SafetySetting{
      .duressEnabled = input.duressEnabled, .updatedAt = at, .updatedBy = input.actorUserId};
}

drogon::Task<DisarmVerdict> SafetyService::authorize(const DisarmRequest& request) const
{
  const SafetySetting setting = co_await settingRepository_.find();
  if (!setting.duressEnabled)
    co_return DisarmVerdict::Allowed;
  const auto pin = co_await pinRepository_.find(request.userId);
  if (!pin)
    co_return DisarmVerdict::Allowed;
  co_return co_await verifyPin({.request = request, .pin = *pin});
}

void SafetyService::duress(const DisarmRequest& request) const
{
  if (!runtime_->alive.load(std::memory_order_acquire))
    return;
  const int64_t at = now();
  runtime_->active.fetch_add(1, std::memory_order_acq_rel);
  drogon::async_run([this, runtime = runtime_, request, at]() -> drogon::Task<> {
    const RuntimeScope scope(runtime);
    try {
      const int64_t alertId = co_await alertRepository_.insert(
          {.kind = SafetyAlertKind::Duress,
           .userId = request.userId,
           .environmentId = request.environmentId.value_or(0),
           .now = at,
           .actorName = request.userName});
      co_await deliver({.kind = SafetyAlertKind::Duress,
                        .alertId = alertId,
                        .actorUserId = request.userId,
                        .actorName = request.userName,
                        .environmentId = request.environmentId.value_or(0),
                        .now = at,
                        .sequence = 1});
    }
    catch (const std::exception& error) {
      LOG_ERROR << "Guard safety: an alert could not be recorded: " << error.what();
    }
  });
}

drogon::Task<PanicResult> SafetyService::panic(const PanicInput& input) const
{
  const int64_t at = now();
  if (const auto repeated = co_await alertRepository_.recent(
          {.kind = SafetyAlertKind::Panic,
           .userId = input.userId,
           .since = at - config_.panicRepeatWindowS}))
    co_return PanicResult{.alertId = repeated->id, .sent = repeated->notified, .repeated = true};
  const SafetyAlertRecentInput limitWindow{.kind = SafetyAlertKind::Panic,
                                           .userId = input.userId,
                                           .since = at - config_.panicLimitWindowS};
  if (co_await alertRepository_.countSince(limitWindow) >= config_.panicLimit) {
    if (const auto latest = co_await alertRepository_.recent(limitWindow))
      co_return PanicResult{.alertId = latest->id, .sent = latest->notified, .repeated = true};
  }
  const int64_t alertId =
      co_await alertRepository_.insert({.kind = SafetyAlertKind::Panic,
                                        .userId = input.userId,
                                        .environmentId = input.environmentId.value_or(0),
                                        .now = at,
                                        .actorName = input.userName});
  const SafetyAlertNotice notice{.kind = SafetyAlertKind::Panic,
                                 .alertId = alertId,
                                 .actorUserId = input.userId,
                                 .actorName = input.userName,
                                 .environmentId = input.environmentId.value_or(0),
                                 .now = at,
                                 .sequence = 1};
  const bool sent = co_await deliver(notice);
  if (dependencies_.actor)
    static_cast<void>(co_await dependencies_.actor->confirmPanic(notice));
  co_return PanicResult{.alertId = alertId, .sent = sent, .repeated = false};
}

int64_t SafetyService::retryDelay(int failures) const
{
  const int step = std::clamp(failures - 1, 0, 16);
  const auto delay = static_cast<int64_t>(
      static_cast<uint64_t>(std::max<int64_t>(1, config_.retryBaseS)) << static_cast<unsigned>(step));
  return std::min(delay, std::max<int64_t>(1, config_.retryMaxS));
}

void SafetyService::recordOutcome(int64_t alertId, bool sent) const
{
  const int64_t at = now();
  std::scoped_lock lock(runtime_->mutex);
  runtime_->delivering.erase(alertId);
  if (sent) {
    runtime_->retries.erase(alertId);
    return;
  }
  SafetyRetry& retry = runtime_->retries[alertId];
  ++retry.failures;
  retry.nextAt = at + retryDelay(retry.failures);
}

drogon::Task<bool> SafetyService::deliver(const SafetyAlertNotice& notice) const
{
  if (!dependencies_.sink)
    co_return false;
  {
    std::scoped_lock lock(runtime_->mutex);
    if (!runtime_->delivering.insert(notice.alertId).second)
      co_return false;
  }
  bool sent = false;
  try {
    const SafetyDelivery delivery = co_await dependencies_.sink->raise(notice);
    sent = delivery == SafetyDelivery::Sent;
    if (sent)
      co_await alertRepository_.markNotified(notice.alertId, now());
    else if (delivery == SafetyDelivery::Refused) {
      LOG_ERROR << "Guard safety: alert " << notice.alertId << " attempt " << notice.sequence
                << " was refused; the next attempt resolves the recipients again";
      static_cast<void>(co_await alertRepository_.advanceSequence(
          {.id = notice.alertId, .sequence = notice.sequence}));
    }
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard safety: alert " << notice.alertId
             << " delivery attempt failed: " << error.what();
    sent = false;
  }
  recordOutcome(notice.alertId, sent);
  co_return sent;
}

drogon::Task<void> SafetyService::escalate(const SafetyAlertRow& alert) const
{
  if (alert.escalatedAt > 0 || alert.createdAt >= now() - config_.resumeWindowS)
    co_return;
  if (co_await alertRepository_.markEscalated(alert.id, now()))
    LOG_ERROR << "Guard safety: " << safetyAlertKindToString(alert.kind) << " alert " << alert.id
              << " of user " << alert.userId << " is still undelivered after "
              << config_.resumeWindowS << " s; it keeps being retried";
}

drogon::Task<size_t> SafetyService::sweepPending() const
{
  size_t delivered = 0;
  const int64_t at = now();
  for (const auto& alert : co_await alertRepository_.pending()) {
    co_await escalate(alert);
    {
      std::scoped_lock lock(runtime_->mutex);
      const auto retry = runtime_->retries.find(alert.id);
      if (retry != runtime_->retries.end() && retry->second.nextAt > at)
        continue;
    }
    if (co_await deliver({.kind = alert.kind,
                          .alertId = alert.id,
                          .actorUserId = alert.userId,
                          .actorName = alert.actorName,
                          .environmentId = alert.environmentId,
                          .now = alert.createdAt,
                          .sequence = alert.sequence}))
      ++delivered;
  }
  co_return delivered;
}

drogon::Task<int64_t> SafetyService::purgeExpired() const
{
  const int64_t at = now();
  {
    std::scoped_lock lock(runtime_->mutex);
    if (runtime_->lastPurgeAt > 0 && at - runtime_->lastPurgeAt < kPurgeIntervalS)
      co_return 0;
    runtime_->lastPurgeAt = at;
  }
  const int64_t removed =
      co_await alertRepository_.purgeBefore(at - std::max(config_.retentionS, config_.resumeWindowS));
  if (removed > 0)
    LOG_INFO << "Guard safety: purged " << removed
             << " delivered alert row(s) past the retention window";
  co_return removed;
}
