#include "safety-service.hxx"

#include <feature/safety/safety-errors.hxx>
#include <feature/safety/services/pin-hash.hxx>

#include <errors/response-exception.hxx>
#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <drogon/drogon.h>
#include <utility>

namespace
{
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
}

SafetyService::SafetyService(Dependencies dependencies, Config config)
    : dependencies_(std::move(dependencies)),
      config_(config),
      attempts_(std::make_shared<PinAttempts>(config.attempts))
{
  if (!dependencies_.clock)
    dependencies_.clock = systemNow;
}

int64_t SafetyService::now() const
{
  return dependencies_.clock();
}

drogon::Task<SafetyStatus> SafetyService::status(int64_t userId) const
{
  const SafetySetting setting = co_await settingRepository_.find();
  const auto pin = co_await pinRepository_.find(userId);
  co_return SafetyStatus{.duressEnabled = setting.duressEnabled,
                         .hasPin = setting.duressEnabled && pin.has_value()};
}

drogon::Task<SafetyStatus> SafetyService::setPin(const PinSetInput& input) const
{
  const SafetySetting setting = co_await settingRepository_.find();
  if (!setting.duressEnabled)
    throw ResponseException(SafetyErrors::DuressDisabled);
  if (input.disarmPin == input.duressPin)
    throw ResponseException(SafetyErrors::PinsMustDiffer);
  const int iterations = config_.pinIterations;
  const auto hashes = co_await BlockingTask<std::pair<std::string, std::string>>{
      [disarm = input.disarmPin, duress = input.duressPin, iterations]() {
        return std::pair{pin_hash::make({.pin = disarm, .iterations = iterations}),
                         pin_hash::make({.pin = duress, .iterations = iterations})};
      }};
  co_await pinRepository_.upsert({.userId = input.userId,
                                  .disarmHash = hashes.first,
                                  .duressHash = hashes.second,
                                  .now = now()});
  attempts_->clear(input.userId);
  co_return SafetyStatus{.duressEnabled = true, .hasPin = true};
}

drogon::Task<SafetyStatus> SafetyService::removePin(int64_t userId) const
{
  co_await pinRepository_.remove(userId);
  attempts_->clear(userId);
  const SafetySetting setting = co_await settingRepository_.find();
  co_return SafetyStatus{.duressEnabled = setting.duressEnabled, .hasPin = false};
}

drogon::Task<SafetySetting> SafetyService::setting() const
{
  co_return co_await settingRepository_.find();
}

drogon::Task<SafetySetting> SafetyService::toggle(const SafetyToggleInput& input) const
{
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
  if (!request.pin || request.pin->empty())
    throw ResponseException(SafetyErrors::PinRequired);
  if (attempts_->locked({.userId = request.userId, .now = now()}))
    throw ResponseException(SafetyErrors::PinLocked);
  const PinMatch match = co_await BlockingTask<PinMatch>{
      [entered = *request.pin, disarm = pin->disarmHash, duress = pin->duressHash]() {
        const bool disarmMatch = pin_hash::verify({.pin = entered, .stored = disarm});
        const bool duressMatch = pin_hash::verify({.pin = entered, .stored = duress});
        return PinMatch{.disarm = disarmMatch, .duress = duressMatch};
      }};
  if (match.duress) {
    attempts_->clear(request.userId);
    co_return DisarmVerdict::Duress;
  }
  if (match.disarm) {
    attempts_->clear(request.userId);
    co_return DisarmVerdict::Allowed;
  }
  attempts_->fail({.userId = request.userId, .now = now()});
  throw ResponseException(SafetyErrors::PinInvalid);
}

void SafetyService::duress(const DisarmRequest& request) const
{
  const int64_t at = now();
  drogon::async_run([this, request, at]() -> drogon::Task<> {
    try {
      const int64_t alertId = co_await alertRepository_.insert(
          {.kind = SafetyAlertKind::Duress,
           .userId = request.userId,
           .environmentId = request.environmentId.value_or(0),
           .now = at});
      co_await deliver({.kind = SafetyAlertKind::Duress,
                        .alertId = alertId,
                        .actorUserId = request.userId,
                        .actorName = request.userName,
                        .environmentId = request.environmentId.value_or(0),
                        .now = at});
    }
    catch (const std::exception& error) {
      LOG_ERROR << "Guard safety: silent alert could not be recorded: " << error.what();
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
    co_return PanicResult{.alertId = *repeated, .sent = true, .repeated = true};
  const int64_t alertId =
      co_await alertRepository_.insert({.kind = SafetyAlertKind::Panic,
                                        .userId = input.userId,
                                        .environmentId = input.environmentId.value_or(0),
                                        .now = at});
  const SafetyAlertNotice notice{.kind = SafetyAlertKind::Panic,
                                 .alertId = alertId,
                                 .actorUserId = input.userId,
                                 .actorName = input.userName,
                                 .environmentId = input.environmentId.value_or(0),
                                 .now = at};
  bool sent = false;
  if (dependencies_.sink)
    sent = co_await dependencies_.sink->raise(notice);
  if (sent)
    co_await alertRepository_.markNotified(alertId, now());
  else
    drogon::async_run([this, notice]() -> drogon::Task<> { co_await deliver(notice); });
  if (dependencies_.actor)
    static_cast<void>(co_await dependencies_.actor->confirmPanic(notice));
  co_return PanicResult{.alertId = alertId, .sent = sent, .repeated = false};
}

drogon::Task<size_t> SafetyService::resumePending() const
{
  size_t delivered = 0;
  for (const auto& alert : co_await alertRepository_.pending(now() - config_.resumeWindowS)) {
    if (co_await deliver({.kind = alert.kind,
                          .alertId = alert.id,
                          .actorUserId = alert.userId,
                          .actorName = {},
                          .environmentId = alert.environmentId,
                          .now = alert.createdAt}))
      ++delivered;
  }
  co_return delivered;
}

drogon::Task<bool> SafetyService::deliver(const SafetyAlertNotice& notice) const
{
  if (!dependencies_.sink)
    co_return false;
  double delay = 2.0;
  for (int attempt = 0; attempt < config_.deliveryAttempts; ++attempt) {
    if (attempt > 0) {
      co_await drogon::sleepCoro(drogon::app().getLoop(), delay);
      delay *= 2.0;
    }
    bool sent = false;
    try {
      sent = co_await dependencies_.sink->raise(notice);
    }
    catch (const std::exception& error) {
      LOG_WARN << "Guard safety: alert " << notice.alertId
               << " delivery attempt failed: " << error.what();
    }
    if (sent) {
      co_await alertRepository_.markNotified(notice.alertId, now());
      co_return true;
    }
  }
  LOG_ERROR << "Guard safety: alert " << notice.alertId << " was not delivered after "
            << config_.deliveryAttempts << " attempts";
  co_return false;
}
