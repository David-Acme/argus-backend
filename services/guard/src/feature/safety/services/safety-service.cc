#include "safety-service.hxx"

#include <feature/safety/safety-errors.hxx>
#include <feature/safety/services/pin-hash.hxx>

#include <errors/response-exception.hxx>
#include <feature/guard/controllers/guard-errors.hxx>
#include <runtime/blocking-task.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <text/secret-phrase.hxx>
#include <trantor/utils/Logger.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <drogon/drogon.h>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
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

bool samePhraseKey(const SafetySetting& setting, const std::string& presented)
{
  return !setting.phraseKey.empty() && setting.phraseKey.size() == presented.size() &&
         CRYPTO_memcmp(setting.phraseKey.data(), presented.data(), presented.size()) == 0;
}

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
                         .hasPin = setting.duressEnabled && pin.has_value(),
                         .hasPhrase = setting.duressEnabled && !setting.phraseKey.empty()};
}

drogon::Task<DisarmVerdict> SafetyService::verifyPin(const PinCheck& check) const
{
  const DisarmRequest& request = check.request;
  if (!request.pin || request.pin->empty())
    throw ResponseException(SafetyErrors::PinRequired);
  const PinReservation reservation =
      co_await attempts_->reserve({.userId = request.userId, .now = now()});
  if (!reservation.admitted)
    throw ResponseException(SafetyErrors::PinLocked);
  PinMatch match;
  try {
    match = co_await BlockingTask<PinMatch>{
        [entered = *request.pin, disarm = check.pin.disarmHash, duress = check.pin.duressHash]() {
          const bool disarmMatch = pin_hash::verify({.pin = entered, .stored = disarm});
          const bool duressMatch = pin_hash::verify({.pin = entered, .stored = duress});
          return PinMatch{.disarm = disarmMatch, .duress = duressMatch};
        },
        BlockingLane::Heavy};
  }
  catch (...) {
    attempts_->release(request.userId);
    throw;
  }
  const bool accepted = !reservation.locked && (match.duress || match.disarm);
  co_await attempts_->settle({.userId = request.userId, .success = accepted});
  if (reservation.locked) {
    if (match.duress)
      duress(request);
    throw ResponseException(SafetyErrors::PinLocked);
  }
  if (match.duress)
    co_return DisarmVerdict::Duress;
  if (match.disarm)
    co_return DisarmVerdict::Allowed;
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
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    co_await pinRepository_.upsert({.userId = input.userId,
                                    .disarmHash = hashes.first,
                                    .duressHash = hashes.second,
                                    .now = now(),
                                    .client = transaction.get()});
    if (dependencies_.changes != nullptr) {
      co_await dependencies_.changes->record(
          {.scope = ResourceScope::SafetyPin,
           .ids = {},
           .user = input.userId,
           .client = transaction.get()});
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(GuardErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_await attempts_->clear(input.userId);
  co_return SafetyStatus{.duressEnabled = true, .hasPin = true, .hasPhrase = !setting.phraseKey.empty()};
}

drogon::Task<SafetyStatus> SafetyService::removePin(const PinRemoveInput& input) const
{
  const DisarmRequest request{.userId = input.userId,
                              .userName = input.userName,
                              .pin = input.currentPin,
                              .environmentId = std::nullopt};
  co_await requireCurrentPin(request);
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    co_await pinRepository_.remove(input.userId, transaction.get());
    if (dependencies_.changes != nullptr) {
      co_await dependencies_.changes->record(
          {.scope = ResourceScope::SafetyPin,
           .ids = {},
           .user = input.userId,
           .client = transaction.get()});
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(GuardErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_await attempts_->clear(input.userId);
  const SafetySetting setting = co_await settingRepository_.find();
  co_return SafetyStatus{.duressEnabled = setting.duressEnabled,
                         .hasPin = false,
                         .hasPhrase = setting.duressEnabled && !setting.phraseKey.empty()};
}

drogon::Task<void> SafetyService::forgetUser(int64_t userId) const
{
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    co_await pinRepository_.remove(userId, transaction.get());
    if (dependencies_.changes != nullptr) {
      co_await dependencies_.changes->record({.scope = ResourceScope::SafetyPin,
                                              .ids = {},
                                              .user = userId,
                                              .client = transaction.get()});
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(GuardErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_await attempts_->clear(userId);
}

drogon::Task<SafetySetting> SafetyService::setting() const
{
  co_return co_await settingRepository_.find();
}

drogon::Task<SafetySetting> SafetyService::toggle(const SafetyToggleInput& input) const
{
  co_return co_await update({.toggle = input, .phraseChange = PhraseChange::Keep, .phrase = {}});
}

drogon::Task<std::string> SafetyService::sealPhrase(const std::string& phrase) const
{
  const int iterations = config_.phraseIterations;
  auto sealed = co_await BlockingTask<std::optional<std::string>>{
      [phrase, iterations]() -> std::optional<std::string> {
        std::string salt(secret_phrase::kSaltBytes, '\0');
        if (RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), static_cast<int>(salt.size())) != 1)
          throw std::runtime_error("phrase salt generation failed");
        const auto key = secret_phrase::seal({.phrase = phrase, .salt = salt, .iterations = iterations});
        if (!key)
          return std::nullopt;
        return secret_phrase::encode(*key);
      },
      BlockingLane::Heavy};
  if (!sealed)
    throw ResponseException(SafetyErrors::PhraseInvalid);
  co_return std::move(*sealed);
}

drogon::Task<SafetySetting> SafetyService::update(const SafetyUpdateInput& input) const
{
  const SafetyToggleInput& toggle = input.toggle;
  const SafetySetting current = co_await settingRepository_.find();
  if (!toggle.duressEnabled && input.phraseChange == PhraseChange::Set)
    throw ResponseException(SafetyErrors::DuressDisabled);
  const bool turningOff = !toggle.duressEnabled && current.duressEnabled;
  const bool phraseChanges = input.phraseChange != PhraseChange::Keep && current.duressEnabled;
  if (turningOff || phraseChanges)
    co_await requireCurrentPin({.userId = toggle.actorUserId,
                                .userName = toggle.actorName,
                                .pin = toggle.currentPin,
                                .environmentId = std::nullopt});
  std::string phraseKey = current.phraseKey;
  if (!toggle.duressEnabled || input.phraseChange == PhraseChange::Clear)
    phraseKey.clear();
  else if (input.phraseChange == PhraseChange::Set)
    phraseKey = co_await sealPhrase(input.phrase);
  const int64_t at = now();
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    co_await settingRepository_.update({.duressEnabled = toggle.duressEnabled,
                                        .updatedBy = toggle.actorUserId,
                                        .now = at,
                                        .client = transaction.get()});
    if (phraseKey != current.phraseKey)
      co_await settingRepository_.setPhrase({.phraseKey = phraseKey,
                                             .updatedBy = toggle.actorUserId,
                                             .now = at,
                                             .client = transaction.get()});
    if (!toggle.duressEnabled)
      co_await pinRepository_.removeAll(transaction.get());
    if (dependencies_.changes != nullptr) {
      co_await dependencies_.changes->record({.scope = ResourceScope::Duress,
                                              .ids = {},
                                              .client = transaction.get()});
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(GuardErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return SafetySetting{.duressEnabled = toggle.duressEnabled,
                          .updatedAt = at,
                          .updatedBy = toggle.actorUserId,
                          .phraseKey = std::move(phraseKey)};
}

drogon::Task<std::string> SafetyService::phraseKey() const
{
  const SafetySetting setting = co_await settingRepository_.find();
  co_return setting.duressEnabled ? setting.phraseKey : std::string{};
}

drogon::Task<SpokenDuressOutcome> SafetyService::spokenDuress(const SpokenDuressInput& input) const
{
  const SafetySetting setting = co_await settingRepository_.find();
  if (!setting.duressEnabled || !samePhraseKey(setting, input.phraseKey))
    co_return SpokenDuressOutcome::Stale;
  const int64_t at = now();
  if (co_await alertRepository_.recentSpoken(
          {.userId = input.userId, .since = at - config_.spokenRepeatWindowS}))
    co_return SpokenDuressOutcome::Repeated;
  const int64_t alertId = co_await alertRepository_.insert({.kind = SafetyAlertKind::Duress,
                                                            .userId = input.userId,
                                                            .environmentId = 0,
                                                            .now = at,
                                                            .actorName = input.userName,
                                                            .spoken = true});
  deliverLater({.kind = SafetyAlertKind::Duress,
                .alertId = alertId,
                .actorUserId = input.userId,
                .actorName = input.userName,
                .environmentId = 0,
                .now = at,
                .sequence = 1,
                .spoken = true});
  co_return SpokenDuressOutcome::Raised;
}

void SafetyService::deliverLater(const SafetyAlertNotice& notice) const
{
  if (!runtime_->alive.load(std::memory_order_acquire))
    return;
  runtime_->active.fetch_add(1, std::memory_order_acq_rel);
  drogon::async_run([this, runtime = runtime_, notice]() -> drogon::Task<> {
    const RuntimeScope scope(runtime);
    try {
      static_cast<void>(co_await deliver(notice));
    }
    catch (const std::exception& error) {
      LOG_WARN << "Guard safety: alert " << notice.alertId << " delivery attempt failed: " << error.what();
    }
  });
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

drogon::Task<OwnerPinOutcome> SafetyService::verifyOwnerPin(DisarmRequest request) const
{
  const SafetySetting setting = co_await settingRepository_.find();
  if (!setting.duressEnabled)
    co_return OwnerPinOutcome::NoPin;
  const auto pin = co_await pinRepository_.find(request.userId);
  if (!pin)
    co_return OwnerPinOutcome::NoPin;
  try {
    if (co_await verifyPin({.request = request, .pin = *pin}) == DisarmVerdict::Duress) {
      duress(request);
      co_return OwnerPinOutcome::Invalid;
    }
    co_return OwnerPinOutcome::Accepted;
  }
  catch (const ResponseException& refusal) {
    if (refusal.errorCode() == ResponseException(SafetyErrors::PinRequired).errorCode())
      co_return OwnerPinOutcome::Required;
    if (refusal.errorCode() == ResponseException(SafetyErrors::PinLocked).errorCode())
      co_return OwnerPinOutcome::Locked;
    if (refusal.errorCode() == ResponseException(SafetyErrors::PinInvalid).errorCode())
      co_return OwnerPinOutcome::Invalid;
    throw;
  }
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
           .actorName = request.userName,
           .spoken = false});
      co_await deliver({.kind = SafetyAlertKind::Duress,
                        .alertId = alertId,
                        .actorUserId = request.userId,
                        .actorName = request.userName,
                        .environmentId = request.environmentId.value_or(0),
                        .now = at,
                        .sequence = 1,
                        .spoken = false});
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
                                        .actorName = input.userName,
                                        .spoken = false});
  const SafetyAlertNotice notice{.kind = SafetyAlertKind::Panic,
                                 .alertId = alertId,
                                 .actorUserId = input.userId,
                                 .actorName = input.userName,
                                 .environmentId = input.environmentId.value_or(0),
                                 .now = at,
                                 .sequence = 1,
                                 .spoken = false};
  const SafetyDelivery delivery = co_await deliver(notice);
  if (dependencies_.actor && delivery != SafetyDelivery::NoRecipients)
    static_cast<void>(co_await dependencies_.actor->confirmPanic(notice));
  co_return PanicResult{.alertId = alertId,
                        .sent = delivery == SafetyDelivery::Sent,
                        .repeated = false};
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

drogon::Task<SafetyDelivery> SafetyService::deliver(const SafetyAlertNotice& notice) const
{
  if (!dependencies_.sink)
    co_return SafetyDelivery::Pending;
  {
    std::scoped_lock lock(runtime_->mutex);
    if (!runtime_->delivering.insert(notice.alertId).second)
      co_return SafetyDelivery::Pending;
  }
  SafetyDelivery delivery = SafetyDelivery::Pending;
  try {
    delivery = co_await dependencies_.sink->raise(notice);
    if (delivery == SafetyDelivery::Sent)
      co_await alertRepository_.markNotified(notice.alertId, now());
    else if (delivery == SafetyDelivery::NoRecipients) {
      LOG_INFO << "Guard safety: alert " << notice.alertId
               << " has nobody left to alert; it is closed and will not be retried";
      if (!co_await alertRepository_.markClosed(notice.alertId, now()))
        delivery = SafetyDelivery::Pending;
    }
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
    delivery = SafetyDelivery::Pending;
  }
  recordOutcome(notice.alertId, delivery != SafetyDelivery::Pending &&
                                    delivery != SafetyDelivery::Refused);
  co_return delivery;
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
                          .sequence = alert.sequence,
                          .spoken = alert.spoken}) == SafetyDelivery::Sent)
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
  static_cast<void>(co_await attempts_->purgeExpired(at));
  co_return removed;
}
