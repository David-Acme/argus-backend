#include "guard-service.hxx"

#include "guard-assessment.hxx"
#include "guard-belief.hxx"
#include "guard-dialogue.hxx"
#include "guard-risk.hxx"

#include <algorithm>
#include <camera/camera-action-client.hxx>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <drogon/drogon.h>
#include <exception>
#include <identity/identity-client.hxx>
#include <notification/notification-client.hxx>
#include <shared/services/storage/s3-storage-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <string>
#include <system_error>
#include <trantor/utils/Logger.h>
#include <unordered_set>
#include <utility>
#include <vector>

constexpr double kRetryReconcileSeconds = 0.5;

// Owns the retry timer so delayed callbacks never hold a raw service pointer.
class ObservationRetryPump
    : public std::enable_shared_from_this<ObservationRetryPump>
{
public:
  void start(std::function<void()> tick)
  {
    tick_ = std::move(tick);
    const std::weak_ptr<ObservationRetryPump> weak = shared_from_this();
    drogon::app().getLoop()->runEvery(kRetryReconcileSeconds, [weak]() {
      if (const auto self = weak.lock())
        self->tick();
    });
    drogon::app().getLoop()->runAfter(0.0, [weak]() {
      if (const auto self = weak.lock())
        self->tick();
    });
  }

  void stop() { tick_ = nullptr; }

  void tick()
  {
    if (tick_)
      tick_();
  }

private:
  std::function<void()> tick_;
};

namespace
{
std::string tagsToJson(const std::vector<std::string>& tags)
{
  Json::Value array(Json::arrayValue);
  for (const auto& tag : tags)
    array.append(tag);
  return json_util::toString(array);
}

int64_t nowMillis()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Health states evidencing physical interference rather than environment:
// re-aimed, occluded or defocused. Darkness alone is a normal night
// condition and never qualifies.
bool tamperIndicating(const std::string& status)
{
  return status == "moved" || status == "covered" || status == "blurred";
}

std::atomic<uint64_t> gCommandSequence{0};
constexpr int kStageIncident = 1;
constexpr int kStageEncounter = 2;
constexpr int kStageDialogue = 3;
constexpr int kStageAssessment = 4;
constexpr int kStageResolve = 5;
constexpr int kStageEffects = 6;
constexpr int kStageEvidence = 7;

constexpr std::string_view kGoalVerify = "verify_identity";
constexpr std::string_view kGoalAwaitReply = "await_reply";
constexpr std::string_view kGoalAwaitHelp = "await_help_reply";
constexpr std::string_view kGoalRepair = "repair_misheard";
constexpr std::string_view kGoalDone = "done";

struct CommandIdInput
{
  std::string correlationId;
  GuardActionKind kind{GuardActionKind::Notify};
  int sequence{0};
  int64_t cameraId{0};
  int64_t now{0};
};

// Deterministic per observation so a redelivery reuses the same camera command
// id.
std::string makeCommandId(const CommandIdInput& input)
{
  const std::string kind = guardActionKindToString(input.kind);
  if (!input.correlationId.empty())
    return input.correlationId + ":" + kind + ":" +
           std::to_string(input.sequence);
  return std::to_string(input.cameraId) + ":" + std::to_string(input.now) +
         ":" + kind + ":" + std::to_string(gCommandSequence.fetch_add(1));
}

struct NotifyBodyInput
{
  std::string cameraName;
  std::string rule;
  GuardDanger danger{GuardDanger::None};
  int64_t cameraId{0};
  int64_t incidentId{0};
  int64_t encounterId{0};
  std::string zoneKind;
  int64_t dwellMs{0};
  IdentityState identity{IdentityState::Unrecognized};
  std::vector<std::string> beliefSignals;
};

struct NotifyBody
{
  std::string title;
  std::string body;
  Json::Value data;
};

NotifyBody buildNotifyBody(const NotifyBodyInput& input)
{
  NotifyBody note;
  note.title = (input.cameraName.empty() ? "Camera" : input.cameraName) +
               ": " + input.rule;
  const std::string who =
      input.identity == IdentityState::Known
          ? "Known person"
          : (input.identity == IdentityState::Unobservable
                 ? "Unidentified person"
                 : "Unrecognized person");
  const std::string where =
      input.zoneKind == "alert"
          ? "the alert zone"
          : (input.zoneKind == "monitor" ? "the monitored area" : "the area");
  note.body = who + " in " + where + " for " +
              std::to_string(std::max<int64_t>(0, input.dwellMs) / 1000) + "s";
  std::vector<std::string> reasons;
  for (const auto& signal : input.beliefSignals) {
    if (reasons.size() >= 3)
      break;
    if (signal.rfind("identity_", 0) == 0)
      continue;
    const std::string phrase = ::beliefSignalPhrase(signal);
    if (!phrase.empty())
      reasons.push_back(phrase);
  }
  if (!reasons.empty()) {
    note.body += " (";
    for (size_t index = 0; index < reasons.size(); ++index) {
      if (index > 0)
        note.body += ", ";
      note.body += reasons[index];
    }
    note.body += ")";
  }
  note.data["cameraId"] = Json::Int64(input.cameraId);
  note.data["rule"] = input.rule;
  note.data["danger"] = guardDangerToString(input.danger);
  note.data["incidentId"] = Json::Int64(input.incidentId);
  note.data["encounterId"] = Json::Int64(input.encounterId);
  note.data["zoneKind"] = input.zoneKind;
  note.data["dwellS"] =
      Json::Int64(std::max<int64_t>(0, input.dwellMs) / 1000);
  note.data["identityState"] = identityStateToString(input.identity);
  return note;
}
} // namespace

struct GuardLifecycle
{
  std::atomic<bool> alive{true};
  std::atomic<int> active{0};
  std::mutex mutex;
  std::condition_variable idle;
};

// One hold per loop callback and event-loop coroutine; the destructor flips
// the flag and waits the holders out, so destruction never races a resume.
class LifecycleGuard
{
public:
  explicit LifecycleGuard(std::shared_ptr<GuardLifecycle> lifecycle)
      : lifecycle_(std::move(lifecycle))
  {
    if (lifecycle_)
      lifecycle_->active.fetch_add(1, std::memory_order_acq_rel);
  }

  ~LifecycleGuard()
  {
    if (!lifecycle_)
      return;
    if (lifecycle_->active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard lock(lifecycle_->mutex);
      lifecycle_->idle.notify_all();
    }
  }

  bool alive() const
  {
    return lifecycle_ != nullptr &&
           lifecycle_->alive.load(std::memory_order_acquire);
  }

private:
  std::shared_ptr<GuardLifecycle> lifecycle_;
};

// One event id executes at a time: a concurrent redelivery defers to the
// owner instead of repeating the saga. The queue already serializes broker
// traffic; this closes the remaining thread-level race.
struct ExecutionLeaseInput
{
  std::mutex& mutex;
  std::unordered_set<std::string>& running;
  std::string id;
};

class ExecutionLease
{
public:
  explicit ExecutionLease(ExecutionLeaseInput input)
      : mutex_(input.mutex), running_(input.running), id_(std::move(input.id))
  {
    std::lock_guard lock(mutex_);
    owned_ = running_.insert(id_).second;
  }

  ~ExecutionLease()
  {
    if (!owned_)
      return;
    std::lock_guard lock(mutex_);
    running_.erase(id_);
  }

  bool owned() const { return owned_; }

private:
  std::mutex& mutex_;
  std::unordered_set<std::string>& running_;
  std::string id_;
  bool owned_{false};
};

GuardService::GuardService(Dependencies dependencies, Config config)
    : dependencies_(dependencies), config_(std::move(config)),
      authorizer_({.notifyLevel = config_.notifyLevel,
                   .announceLevel = config_.announceLevel,
                   .alarmLevel = config_.alarmLevel,
                   .armSiren = config_.armSiren,
                   .dialogueEnabled = config_.greetEnabled}),
      lifecycle_(std::make_shared<GuardLifecycle>())
{
  retryPump_ = std::make_shared<ObservationRetryPump>();
}

GuardService::~GuardService()
{
  lifecycle_->alive.store(false, std::memory_order_release);
  if (retryPump_)
    retryPump_->stop();
  if (dependencies_.bus) {
    if (durableSubscription_.has_value())
      dependencies_.bus->unsubscribe(*durableSubscription_);
    if (advisorySubscription_.has_value())
      dependencies_.bus->unsubscribe(*advisorySubscription_);
    if (healthSubscription_.has_value())
      dependencies_.bus->unsubscribe(*healthSubscription_);
  }
  stopTimers();
  std::unique_lock lock(lifecycle_->mutex);
  lifecycle_->idle.wait(lock, [lifecycle = lifecycle_] {
    return lifecycle->active.load(std::memory_order_acquire) == 0;
  });
}

void GuardService::trackTimer(uint64_t id)
{
  std::lock_guard lock(lifecycleMutex_);
  timerIds_.push_back(id);
}

void GuardService::stopTimers()
{
  std::vector<uint64_t> ids;
  {
    std::lock_guard lock(lifecycleMutex_);
    ids = std::move(timerIds_);
  }
  if (!drogon::app().isRunning())
    return;
  for (const auto id : ids)
    drogon::app().getLoop()->invalidateTimer(id);
}

void GuardService::start()
{
  if (!config_.enabled)
    return;

  startSweeps();
  if (retryPump_)
    retryPump_->start([this, lifecycle = lifecycle_]() {
      drogon::async_run(
          [this, lifecycle]() -> drogon::Task<void> {
            const LifecycleGuard guard(lifecycle);
            if (!guard.alive())
              co_return;
            try {
              co_await reconcileObservations();
            }
            catch (const std::exception& error) {
              LOG_WARN << "Guard service: observation reconcile failed: "
                       << error.what();
            }
            catch (...) {
              LOG_WARN << "Guard service: observation reconcile failed with "
                          "unknown error";
            }
            co_return;
          });
    });
  if (!dependencies_.bus)
    return;

  if (!trySubscribe()) {
    LOG_WARN << "Guard service: durable consumer unavailable (camera stream "
                "not ready?); retrying";
    scheduleSubscribeRetry();
    return;
  }
  startHeartbeat();
  publishHeartbeat();
  LOG_INFO << "Guard service: watching "
           << (config_.eventSubject.empty()
                   ? std::string(nats_subject::kCameraObjectDetected)
                   : config_.eventSubject)
           << " (profile " << config_.profile << ", durable "
           << config_.consumerDurable << ")";
}

void GuardService::startHeartbeat()
{
  if (heartbeatStarted_)
    return;
  heartbeatStarted_ = true;
  trackTimer(drogon::app().getLoop()->runEvery(
      static_cast<double>(std::max(1, config_.heartbeatS)),
      [this, lifecycle = lifecycle_]() {
        const LifecycleGuard guard(lifecycle);
        if (!guard.alive())
          return;
        publishHeartbeat();
      }));
}

drogon::Task<void> GuardService::reconcileObservations()
{
  const LifecycleGuard guard(lifecycle_);
  if (!guard.alive())
    co_return;
  const int64_t nowMs = nowMillis();
  const auto due = co_await repository_.dueObservations(nowMs);
  for (const auto& row : due) {
    const bool claimed = co_await repository_.claimInboxRetry(
        {.eventId = row.eventId, .leaseUntil = nowMs + config_.retryLeaseMs,
         .now = nowMs});
    if (!claimed)
      continue;
    enqueue({.payload = row.payload, .delivered = 0, .leased = true});
  }
  co_return;
}

bool GuardService::trySubscribe()
{
  constexpr int64_t kGuardStreamRetentionNs = 7LL * 24 * 60 * 60 * 1000000000;
  constexpr int64_t kGuardStreamDuplicatesNs = 2LL * 60 * 1000000000;
  // Owned here so encounter_closed survives restarts for a durable consumer.
  if (!dependencies_.bus->ensureStream(
          {.name = config_.guardStream,
           .subjects = {"argus.guard.v1.>"},
           .maxAgeNs = kGuardStreamRetentionNs,
           .duplicatesNs = kGuardStreamDuplicatesNs}))
    return false;
  const auto subscription = dependencies_.bus->subscribeDurable(
      {.stream = config_.eventStream,
       .durable = config_.consumerDurable,
       .subject = config_.eventSubject.empty()
                      ? std::string(nats_subject::kCameraObjectDetected)
                      : config_.eventSubject,
       .deliverAll = true,
       .maxDeliver = config_.maxObservationAttempts,
        .handler = [this, lifecycle = lifecycle_](
                         const NatsBus::DurableMessage& message,
                         NatsBus::DurableSettlement settlement) {
          drogon::app().getIOLoop(0)->runInLoop(
              [this, lifecycle, payload = std::string(message.payload),
               settlement = std::move(settlement),
               delivered = message.delivered]() mutable {
                const LifecycleGuard guard(lifecycle);
                if (!guard.alive()) {
                  if (settlement.nak)
                    settlement.nak();
                  return;
                }
                enqueue({.payload = std::move(payload),
                         .ack = std::move(settlement.ack),
                         .nak = std::move(settlement.nak),
                         .term = std::move(settlement.term),
                         .delivered = delivered});
              });
        }});
  if (!subscription)
    return false;
  durableSubscription_ = *subscription;
  subscribed_ = true;
  subscribeAdvisories();
  subscribeHealth();
  return true;
}

void GuardService::failAt(const std::string& name) const
{
  if (config_.failPoint && config_.failPoint(name))
    throw std::runtime_error("failpoint: " + name);
}

std::string GuardService::sanitizeSpoken(const std::string& text) const
{
  return guard_dialogue::sanitizeLine({.text = text,
                                       .maxWords = config_.maxAnnounceWords,
                                       .privateTokens = {}});
}

void GuardService::subscribeAdvisories()
{
  if (advisoriesSubscribed_ || !dependencies_.bus)
    return;
  const std::string subject =
      "$JS.EVENT.ADVISORY.CONSUMER.MAX_DELIVERIES.ARGUS_CAMERA." +
      config_.consumerDurable;
  const auto advisory = dependencies_.bus->subscribe(
      subject, [](std::string_view, std::string_view payload) {
        LOG_ERROR << "Guard service: JetStream max-deliveries advisory "
                  << payload.substr(0, 240);
      });
  if (advisory) {
    advisorySubscription_ = *advisory;
    advisoriesSubscribed_ = true;
  }
}

void GuardService::subscribeHealth()
{
  if (healthSubscribed_ || !dependencies_.bus)
    return;
  const auto subscription = dependencies_.bus->subscribe(
      std::string(nats_subject::kCameraHealth),
      [this](std::string_view, std::string_view payload) {
        const Json::Value health =
            json_util::fromString(std::string(payload));
        if (!health.isObject())
          return;
        const int64_t cameraId = health.get("cameraId", 0).asInt64();
        if (cameraId <= 0)
          return;
        ingestHealth(cameraId, health.get("status", "").asString(),
                     nowMillis());
      });
  if (subscription) {
    healthSubscription_ = *subscription;
    healthSubscribed_ = true;
  }
}

void GuardService::ingestHealth(int64_t cameraId, const std::string& status,
                                int64_t atMs)
{
  if (cameraId <= 0)
    return;
  std::lock_guard lock(healthMutex_);
  CameraHealth& health = healthByCamera_[cameraId];
  if (health.status == status && health.lastSeenMs > 0) {
    health.lastSeenMs = atMs;
    return;
  }
  health = {.status = status, .firstSeenMs = atMs, .lastSeenMs = atMs};
}

std::string GuardService::cameraHealthStatus(int64_t cameraId) const
{
  std::lock_guard lock(healthMutex_);
  const auto found = healthByCamera_.find(cameraId);
  if (found == healthByCamera_.end())
    return {};
  if (nowMillis() - found->second.lastSeenMs > config_.healthStaleS * 1000)
    return {};
  return found->second.status;
}

drogon::Task<void> GuardService::checkTamperSweep(int64_t now)
{
  struct TamperReading
  {
    int64_t cameraId{0};
    std::string status;
    int64_t firstSeenMs{0};
    int64_t lastSeenMs{0};
  };
  std::vector<TamperReading> fresh;
  {
    std::lock_guard lock(healthMutex_);
    for (const auto& [cameraId, health] : healthByCamera_) {
      if (now - health.lastSeenMs / 1000 > config_.healthStaleS)
        continue;
      fresh.push_back({.cameraId = cameraId,
                       .status = health.status,
                       .firstSeenMs = health.firstSeenMs,
                       .lastSeenMs = health.lastSeenMs});
    }
  }
  const auto readStamp =
      [this](const std::string& key) -> drogon::Task<int64_t> {
    const std::string stored = co_await repository_.state(key, "");
    int64_t stamp = 0;
    const auto [end, error] =
        std::from_chars(stored.data(), stored.data() + stored.size(), stamp);
    if (error != std::errc{} || end != stored.data() + stored.size())
      co_return 0;
    co_return stamp;
  };
  for (const auto& reading : fresh) {
    const std::string onsetKey =
        "tamper_onset_" + std::to_string(reading.cameraId);
    const std::string seenKey =
        "tamper_last_seen_" + std::to_string(reading.cameraId);
    const std::string notifiedKey =
        "tamper_notified_onset_" + std::to_string(reading.cameraId);
    if (!tamperIndicating(reading.status)) {
      if (co_await readStamp(onsetKey) > 0 &&
          now - co_await readStamp(seenKey) >= config_.tamperSustainedS) {
        co_await repository_.clearState(onsetKey);
        co_await repository_.clearState(seenKey);
        co_await repository_.clearState(notifiedKey);
      }
      continue;
    }
    int64_t onset = co_await readStamp(onsetKey);
    if (onset <= 0) {
      onset = now;
      co_await repository_.setState(
          {.key = onsetKey, .value = std::to_string(now), .updatedAt = now});
    }
    co_await repository_.setState(
        {.key = seenKey, .value = std::to_string(now), .updatedAt = now});
    if (now - onset < config_.tamperSustainedS)
      continue;
    if (co_await repository_.state(notifiedKey, "") ==
        std::to_string(onset))
      continue;
    const std::string eventId = "tamper:" + std::to_string(reading.cameraId) +
                                ":" + std::to_string(now);
    const int64_t incidentId =
        co_await repository_.insertIncidentForEvent(
            {.cameraId = reading.cameraId,
             .cameraName = {},
             .rule = "camera_tamper",
             .danger = guard_policy::dangerToString(GuardDanger::High),
             .severity = "high",
             .personId = 0,
             .identity = {},
             .eventId = eventId,
             .eventJson = {},
             .createdAt = now});
    const NotifyBody content = buildNotifyBody(
        {.cameraName = {},
         .rule = "camera_tamper",
         .danger = GuardDanger::High,
         .cameraId = reading.cameraId,
         .incidentId = incidentId,
         .encounterId = 0,
         .zoneKind = {},
         .dwellMs = 0,
         .identity = IdentityState::Unobservable,
         .beliefSignals = {"camera_health_degraded"}});
    co_await journalDecision(
        {.eventId = eventId,
         .encounterId = 0,
         .incidentId = incidentId,
         .cameraId = reading.cameraId,
         .observationId = {},
         .danger = GuardDanger::High,
         .hardFloor = false,
         .beliefScore = 0,
         .beliefSignals = {"camera_health_degraded"},
         .beliefThreshold = 0,
         .legacyWouldNotify = true,
         .beliefWouldNotify = false,
         .decisionMode = config_.decisionMode,
         .suppression = DecisionSuppression::None,
         .suppressedKinds = {},
         .noveltyScore = 0.0,
         .repeatVisits = 0,
         .quietHold = false,
         .budgetHold = false,
         .assessMs = 0,
         .at = now});
    const EffectResult sent = co_await performEffect(
        {.kind = GuardActionKind::Notify,
         .danger = GuardDanger::High,
         .greetingEnabled = false,
         .replyRequested = false,
         .cameraId = reading.cameraId,
         .cameraName = {},
         .rule = "camera_tamper",
         .incidentId = incidentId,
         .encounterId = 0,
         .personId = 0,
         .now = now,
         .text = {},
         .lang = {},
         .seconds = 0,
         .correlationId = eventId,
         .sequence = 1,
         .notifyContent = {.title = content.title,
                           .body = content.body,
                           .data = content.data}});
    if (!sent.accepted)
      continue;
    co_await repository_.setState({.key = notifiedKey,
                                   .value = std::to_string(onset),
                                   .updatedAt = now});
  }
  co_return;
}

BeliefConfig GuardService::beliefConfig(int64_t cameraId) const
{
  const int64_t now = nowMillis();
  {
    std::lock_guard lock(beliefMutex_);
    const auto found = beliefCache_.find(cameraId);
    if (found != beliefCache_.end() &&
        now - found->second.resolvedAt < config_.beliefRefreshS * 1000)
      return found->second.config;
  }
  BeliefConfig resolved = guard_belief::resolveBeliefConfig(cameraId);
  {
    std::lock_guard lock(beliefMutex_);
    beliefCache_[cameraId] = {.config = resolved, .resolvedAt = nowMillis()};
  }
  return resolved;
}

drogon::Task<bool>
GuardService::journalDecision(const JournalDecisionInput& input)
{
  try {
    failAt("journal_write");
    Json::Value signals(Json::arrayValue);
    for (const auto& name : input.beliefSignals)
      signals.append(name);
    Json::Value kinds(Json::arrayValue);
    for (const auto& kind : input.suppressedKinds)
      kinds.append(kind);
    co_return co_await repository_.insertDecisionJournal(
        {.eventId = input.eventId,
         .encounterId = input.encounterId,
         .incidentId = input.incidentId,
         .cameraId = input.cameraId,
         .observationId = input.observationId,
         .severity = guard_policy::dangerToString(input.danger),
         .severityRank = guard_policy::dangerRank(input.danger),
         .hardFloor = input.hardFloor,
         .beliefScore = input.beliefScore,
         .beliefSignals = json_util::toString(signals),
         .beliefThreshold = input.beliefThreshold,
         .legacyWouldNotify = input.legacyWouldNotify,
         .beliefWouldNotify = input.beliefWouldNotify,
         .didNotify = false,
         .decisionMode = input.decisionMode,
         .suppression = input.suppression,
         .suppressedKinds = json_util::toString(kinds),
         .noveltyScore = input.noveltyScore,
         .repeatVisits = input.repeatVisits,
         .quietHold = input.quietHold,
         .budgetHold = input.budgetHold,
         .assessMs = input.assessMs,
         .createdAt = input.at});
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard service: decision journal write failed, saga "
                "continues: "
             << error.what();
  }
  catch (...) {
    LOG_WARN << "Guard service: decision journal write failed with unknown "
                "error, saga continues";
  }
  co_return false;
}

drogon::Task<GuardService::CollectionResult>
GuardService::collectSignals(const CollectionInput& input)
{
  CollectionResult collected;
  try {
    std::tm parts{};
    const std::time_t at = static_cast<std::time_t>(input.now);
    if (gmtime_r(&at, &parts) == nullptr)
      co_return collected;
    const int dowHour = parts.tm_wday * 24 + parts.tm_hour;
    const BaselineEmaRow baseline =
        co_await repository_.baselineEma(input.cameraId, dowHour);
    const double decayed = guard_policy::decayBaseline(
        baseline.ema, input.now - baseline.updatedAt);
    collected.noveltyScore = guard_policy::baselineNovelty(decayed);
    co_await repository_.upsertBaselineEma(
        {.cameraId = input.cameraId,
         .dowHour = dowHour,
         .ema = decayed + 1.0,
         .at = input.now});
    if (input.hasUnknown && !input.signature.empty())
      collected.repeatVisits =
          co_await repository_.touchSignatureVisit(input.signature, input.now);
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard service: calibration collection failed: "
             << error.what();
  }
  catch (...) {
    LOG_WARN << "Guard service: calibration collection failed with unknown "
                "error";
  }
  co_return collected;
}

drogon::Task<GuardService::HoldResult>
GuardService::computeHolds(const HoldInput& input)
{
  HoldResult holds;
  try {
    if (!config_.quietHoursEnabled || !input.legacyWouldNotify)
      co_return holds;
    if (guard_policy::dangerRank(input.danger) >=
        guard_policy::dangerRank(GuardDanger::High))
      co_return holds;
    std::tm parts{};
    const std::time_t at = static_cast<std::time_t>(input.now);
    if (localtime_r(&at, &parts) == nullptr)
      co_return holds;
    const int hour = parts.tm_hour;
    const bool inWindow = config_.quietStartHour <= config_.quietEndHour
                              ? (hour >= config_.quietStartHour &&
                                 hour < config_.quietEndHour)
                              : (hour >= config_.quietStartHour ||
                                 hour < config_.quietEndHour);
    if (inWindow)
      holds.quiet = true;
    const int64_t midnight =
        input.now - (parts.tm_hour * 3600 + parts.tm_min * 60 + parts.tm_sec);
    if (co_await repository_.firedSince(midnight) >= config_.quietDailyBudget)
      holds.budget = true;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard service: hold marker computation failed: "
             << error.what();
  }
  catch (...) {
    LOG_WARN << "Guard service: hold marker computation failed with unknown "
                "error";
  }
  co_return holds;
}

void GuardService::scheduleSubscribeRetry()
{
  trackTimer(drogon::app().getLoop()->runEvery(5.0, [this, lifecycle = lifecycle_]() {
    const LifecycleGuard guard(lifecycle);
    if (!guard.alive() || subscribed_)
      return;
    if (trySubscribe()) {
      LOG_INFO << "Guard service: durable consumer connected (attempt "
               << subscribeAttempts_ + 1 << ")";
      startHeartbeat();
      publishHeartbeat();
      startSweeps();
      return;
    }
    if (++subscribeAttempts_ == 60)
      LOG_WARN << "Guard service: still without a durable consumer; camera "
                  "events will replay once the stream is available";
  }));
}

void GuardService::startSweeps()
{
  if (sweepsStarted_)
    return;
  sweepsStarted_ = true;
  scheduleEncounterSweep();
  scheduleRetentionSweep();
}

int64_t GuardService::retryBackoffAt(int attempts, int64_t nowMs) const
{
  const int boundedStep = std::clamp(attempts > 0 ? attempts - 1 : 0, 0, 16);
  int64_t delay = static_cast<int64_t>(config_.retryBaseMs) << boundedStep;
  if (config_.retryMaxMs > 0)
    delay = std::min<int64_t>(delay, config_.retryMaxMs);
  return nowMs + std::max<int64_t>(delay, 1);
}

void GuardService::enqueue(QueueEntry entry)
{
  {
    std::lock_guard lock(queueMutex_);
    queue_.push_back(std::move(entry));
    if (processing_)
      return;
    processing_ = true;
  }
  drogon::async_run(
      [this]() -> drogon::Task<void> { co_await processQueue(); });
}

drogon::Task<void> GuardService::processQueue()
{
  const LifecycleGuard aliveGuard(lifecycle_);
  while (true) {
    QueueEntry entry;
    {
      std::lock_guard lock(queueMutex_);
      if (queue_.empty()) {
        processing_ = false;
        co_return;
      }
      entry = std::move(queue_.front());
      queue_.pop_front();
    }
    if (!aliveGuard.alive()) {
      if (entry.nak)
        entry.nak();
      continue;
    }
    try {
      const Json::Value event = json_util::fromString(entry.payload);
      if (!event.isObject()) {
        LOG_WARN << "Guard service: malformed observation discarded";
        if (entry.term)
          entry.term();
        continue;
      }
      co_await handleEvent({.event = event,
                             .delivered = entry.delivered,
                             .leased = entry.leased});
      if (entry.ack)
        entry.ack();
    }
    catch (const std::exception& e) {
      LOG_WARN << "Guard service: observation failed; will redeliver ("
               << e.what() << ")";
      if (entry.nak)
        entry.nak();
    }
    catch (...) {
      LOG_WARN << "Guard service: observation failed; will redeliver "
                  "(unknown error)";
      if (entry.nak)
        entry.nak();
    }
  }
}

void GuardService::publishHeartbeat()
{
  if (!dependencies_.bus)
    return;
  Json::Value payload;
  payload["service"] = "argus-guard";
  payload["enabled"] = config_.enabled;
  payload["at"] = Json::Int64(std::time(nullptr));
  dependencies_.bus->publish(nats_subject::kGuardHeartbeat,
                             json_util::toString(payload));
}

void GuardService::scheduleEncounterSweep()
{
  trackTimer(drogon::app().getLoop()->runEvery(60.0, [this, lifecycle = lifecycle_]() {
    drogon::async_run([this, lifecycle]() -> drogon::Task<void> {
      const LifecycleGuard guard(lifecycle);
      if (!guard.alive())
        co_return;
      try {
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const auto stale = co_await repository_.staleEncounters(
            now - config_.encounterTimeoutS);
        if (!stale.empty()) {
          co_await repository_.closeStaleEncounters(
              {.olderThan = now - config_.encounterTimeoutS,
               .closedAt = now,
               .decisionMode = config_.decisionMode});
        }
        co_await flushEncounterOutbox();
        co_await checkTamperSweep(now);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Guard service: encounter sweep failed: " << error.what();
      }
      catch (...) {
        LOG_WARN << "Guard service: encounter sweep failed with unknown error";
      }
      co_return;
    });
  }));
}

void GuardService::publishEncounterClosed(const GuardEncounter& encounter,
                                          int64_t at)
{
  // Enqueue is durable and bus-independent; only the publish needs the bus.
  const EncounterOutboxInput input{.eventId =
                                       encounterClosedEventId(encounter, at),
                                   .payload =
                                       encounterClosedPayload(encounter, at),
                                   .at = at};
  drogon::async_run([this, input, lifecycle = lifecycle_]() -> drogon::Task<void> {
    const LifecycleGuard guard(lifecycle);
    if (!guard.alive())
      co_return;
    try {
      co_await repository_.enqueueEncounterClosed(input);
      co_await flushEncounterOutbox();
    }
    catch (const std::exception& error) {
      LOG_WARN << "Guard service: encounter close publish failed: "
               << error.what();
    }
    catch (...) {
      LOG_WARN << "Guard service: encounter close publish failed with "
                  "unknown error";
    }
    co_return;
  });
}

drogon::Task<void> GuardService::flushEncounterOutbox()
{
  if (!dependencies_.bus)
    co_return;
  const auto pending = co_await repository_.pendingEncounterClosed();
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  for (const auto& row : pending) {
    const bool published = dependencies_.bus->publishWithMsgId(
        {.subject = nats_subject::kGuardEncounterClosed,
         .payload = row.payload,
         .msgId = row.eventId});
    if (published)
      co_await repository_.markEncounterSent(row.eventId, now);
    else
      co_await repository_.recordEncounterAttempt(row.eventId, now);
  }
  co_return;
}

drogon::Task<bool> GuardService::handle(const Json::Value& event, int delivered)
{
  co_return co_await handleEvent(
      {.event = event, .delivered = delivered, .leased = false});
}

drogon::Task<bool> GuardService::handleLocalRetry(const Json::Value& event)
{
  co_return co_await handleEvent(
      {.event = event, .delivered = 0, .leased = true});
}

drogon::Task<bool>
GuardService::handleEvent(HandleEventInput input)
{
  const LifecycleGuard aliveGuard(lifecycle_);
  if (!aliveGuard.alive())
    co_return true;
  const GuardEventSignals signals =
      guard_policy::parseObjectEvent(input.event);
  if (signals.cameraId <= 0 || signals.rule.empty())
    co_return true;

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string eventId = input.event.get("eventId", "").asString();
  if (eventId.empty()) {
    co_await applyObservation(
        {.event = input.event, .signals = signals, .state = {}});
    co_return true;
  }

  const ExecutionLease lease(
      {.mutex = lifecycleMutex_, .running = executing_, .id = eventId});
  if (!lease.owned()) {
    LOG_INFO << "Guard service: observation " << eventId
             << " already executing; deferring to its owner";
    co_return true;
  }
  const std::string payload = json_util::toString(input.event);
  ObservationClaim state = co_await repository_.claimObservation(
      {.eventId = eventId,
       .cameraId = signals.cameraId,
       .observationId = signals.observationId,
       .receivedAt = now,
       .payload = payload,
       .delivered = input.delivered});
  if (state.kind == ObservationClaimKind::Completed) {
    LOG_INFO << "Guard service: duplicate observation " << eventId
             << " ignored";
    co_return true;
  }
  if (state.kind == ObservationClaimKind::Scheduled && !input.leased) {
    LOG_INFO << "Guard service: observation " << eventId
             << " already owned by its durable retry";
    co_return true;
  }
  if (state.kind == ObservationClaimKind::Scheduled)
    state.kind = ObservationClaimKind::Retry;
  const int parkThreshold = std::max(1, config_.maxObservationAttempts - 1);
  if (!input.leased && state.attempts >= parkThreshold) {
    const bool parked = co_await repository_.parkObservation(
        {.letter = {.eventId = eventId,
                    .payload = payload,
                    .reason = "max_deliveries",
                    .attempts = state.attempts,
                    .at = now},
         .at = now});
    if (!parked)
      throw std::runtime_error("dead-letter park failed");
    LOG_ERROR << "Guard service: observation " << eventId
              << " parked in the dead-letter table after " << state.attempts
              << " deliveries";
    co_return true;
  }
  failAt("after_claim");
  const ObservationResult outcome = co_await applyObservation(
      {.event = input.event, .signals = signals, .state = std::move(state)});
  if (!outcome.completed) {
    const int64_t retryAt =
        outcome.retryAt > 0
            ? outcome.retryAt
            : retryBackoffAt(state.localRetries + 1, nowMillis());
    const bool scheduled = co_await repository_.setInboxRetry(
        {.eventId = eventId,
         .payload = payload,
         .retryAt = retryAt,
         .at = now});
    if (!scheduled)
      throw std::runtime_error("observation retry persist failed");
    failAt("after_schedule");
    co_return true;
  }
  failAt("before_complete");
  if (!co_await repository_.completeObservation(eventId, now))
    throw std::runtime_error("observation completion failed");
  co_return true;
}

Json::Value
GuardService::checkpointToJson(const ObservationCheckpoint& checkpoint)
{
  Json::Value json(Json::objectValue);
  json["encounterChecks"] = checkpoint.encounterChecks;
  json["visitCount"] = checkpoint.visitCount;
  json["expectedGuest"] = checkpoint.expectedGuest;
  json["guestId"] = Json::Int64(checkpoint.guestId);
  json["guestOneTime"] = checkpoint.guestOneTime;
  json["hardFloor"] = checkpoint.hardFloor;
  json["observedOnly"] = checkpoint.observedOnly;
  json["effectsDenied"] = checkpoint.effectsDenied;
  json["effectsStarted"] = checkpoint.effectsStarted;
  json["policyDanger"] = checkpoint.policyDanger;
  json["danger"] = checkpoint.danger;
  json["greetingStatus"] = checkpoint.greetingStatus;
  json["greetingDetail"] = checkpoint.greetingDetail;

  Json::Value dialogue(Json::objectValue);
  dialogue["greeted"] = checkpoint.dialogue.greeted;
  dialogue["greetingText"] = checkpoint.dialogue.greetingText;
  dialogue["listened"] = checkpoint.dialogue.listened;
  dialogue["heardText"] = checkpoint.dialogue.heardText;
  dialogue["replied"] = checkpoint.dialogue.replied;
  dialogue["replyText"] = checkpoint.dialogue.replyText;
  dialogue["repairPending"] = checkpoint.dialogue.repairPending;
  dialogue["listenAfterReply"] = checkpoint.dialogue.listenAfterReply;
  dialogue["turns"] = checkpoint.dialogue.turns;
  json["dialogue"] = std::move(dialogue);

  Json::Value assessment(Json::objectValue);
  assessment["performed"] = checkpoint.assessment.performed;
  assessment["valid"] = checkpoint.assessment.valid;
  assessment["veto"] = checkpoint.assessment.veto;
  assessment["mode"] = checkpoint.assessment.mode;
  assessment["threat"] = checkpoint.assessment.threat;
  assessment["caption"] = checkpoint.assessment.caption;
  assessment["summary"] = checkpoint.assessment.summary;
  assessment["announceText"] = checkpoint.assessment.announceText;
  assessment["toolRounds"] = checkpoint.assessment.toolRounds;
  Json::Value tags(Json::arrayValue);
  for (const auto& tag : checkpoint.assessment.tags)
    tags.append(tag);
  assessment["tags"] = std::move(tags);
  Json::Value toolLogs(Json::arrayValue);
  for (const auto& log : checkpoint.assessment.toolLogs) {
    Json::Value entry(Json::objectValue);
    entry["kind"] = log.kind;
    entry["status"] = log.status;
    toolLogs.append(std::move(entry));
  }
  assessment["toolLogs"] = std::move(toolLogs);
  json["assessment"] = std::move(assessment);
  return json;
}

GuardService::ObservationCheckpoint
GuardService::checkpointFromJson(const Json::Value& json)
{
  ObservationCheckpoint checkpoint;
  if (!json.isObject())
    return checkpoint;
  checkpoint.encounterChecks = json.get("encounterChecks", 0).asInt();
  checkpoint.visitCount = json.get("visitCount", 0).asInt();
  checkpoint.expectedGuest = json.get("expectedGuest", false).asBool();
  checkpoint.guestId = json.get("guestId", 0).asInt64();
  checkpoint.guestOneTime = json.get("guestOneTime", false).asBool();
  checkpoint.hardFloor = json.get("hardFloor", false).asBool();
  checkpoint.observedOnly = json.get("observedOnly", false).asBool();
  checkpoint.effectsDenied = json.get("effectsDenied", false).asBool();
  checkpoint.effectsStarted = json.get("effectsStarted", false).asBool();
  checkpoint.policyDanger = json.get("policyDanger", "").asString();
  checkpoint.danger = json.get("danger", "").asString();
  checkpoint.greetingStatus = json.get("greetingStatus", "").asString();
  checkpoint.greetingDetail = json.get("greetingDetail", "").asString();

  const Json::Value& dialogue = json["dialogue"];
  if (dialogue.isObject()) {
    checkpoint.dialogue.greeted = dialogue.get("greeted", false).asBool();
    checkpoint.dialogue.greetingText =
        dialogue.get("greetingText", "").asString();
    checkpoint.dialogue.listened = dialogue.get("listened", false).asBool();
    checkpoint.dialogue.heardText = dialogue.get("heardText", "").asString();
    checkpoint.dialogue.replied = dialogue.get("replied", false).asBool();
    checkpoint.dialogue.replyText = dialogue.get("replyText", "").asString();
    checkpoint.dialogue.repairPending =
        dialogue.get("repairPending", false).asBool();
    checkpoint.dialogue.listenAfterReply =
        dialogue.get("listenAfterReply", false).asBool();
    checkpoint.dialogue.turns = dialogue.get("turns", 0).asInt();
  }

  const Json::Value& assessment = json["assessment"];
  if (assessment.isObject()) {
    checkpoint.assessment.performed =
        assessment.get("performed", false).asBool();
    checkpoint.assessment.valid = assessment.get("valid", false).asBool();
    checkpoint.assessment.veto = assessment.get("veto", false).asBool();
    checkpoint.assessment.mode = assessment.get("mode", "").asString();
    checkpoint.assessment.threat = assessment.get("threat", "").asString();
    checkpoint.assessment.caption = assessment.get("caption", "").asString();
    checkpoint.assessment.summary = assessment.get("summary", "").asString();
    checkpoint.assessment.announceText =
        assessment.get("announceText", "").asString();
    checkpoint.assessment.toolRounds = assessment.get("toolRounds", 0).asInt();
    for (const auto& tag : assessment["tags"]) {
      if (tag.isString())
        checkpoint.assessment.tags.push_back(tag.asString());
    }
    for (const auto& entry : assessment["toolLogs"]) {
      if (entry.isObject())
        checkpoint.assessment.toolLogs.push_back(
            {.kind = entry.get("kind", "").asString(),
             .status = entry.get("status", "").asString()});
    }
  }
  return checkpoint;
}

drogon::Task<bool> GuardService::advanceObservation(const AdvanceInput& input)
{
  co_return co_await repository_.advanceObservation(
      {.eventId = input.eventId,
       .stage = input.stage,
       .incidentId = input.incidentId,
       .encounterId = input.encounterId,
       .danger = input.checkpoint.danger,
       .checkpoint = json_util::toString(checkpointToJson(input.checkpoint)),
       .at = input.at});
}

drogon::Task<GuardService::ObservationResult>
GuardService::applyObservation(const ObservationInput& input)
{
  const Json::Value& event = input.event;
  const GuardEventSignals& signals = input.signals;
  ObservationClaim state = std::move(input.state);
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string eventId = event.get("eventId", "").asString();

  ObservationCheckpoint checkpoint =
      checkpointFromJson(json_util::fromString(state.checkpoint));

  const GuardMode mode = guard_policy::modeFromString(
      co_await repository_.state("mode", guard_policy::modeToString(
                                             config_.defaultMode)));

  GuardDanger danger = GuardDanger::None;
  bool hardFloor = checkpoint.hardFloor;
  int assessMs = 0;
  if (state.stage == 0) {
    GuardContext context;
    context.mode = mode;
    context.rule = signals.rule;
    context.severity = signals.severity;
    context.hasKnown = signals.hasKnown;
    context.hasUnknown = signals.hasUnknown;
    context.inAlertZone =
        signals.zoneKind == "alert" || signals.rule == "person_in_alert_zone";
    context.atNight =
        signals.rule == "person_night" || signals.rule == "vehicle_night";
    context.escalated = signals.escalated;
    if (signals.personId > 0)
      context.visitCount =
          co_await repository_.repeatCount(signals.personId,
                                           now - config_.repeatWindowS);
    if (config_.expectedGuestsEnabled && signals.hasUnknown) {
      const auto guest =
          co_await repository_.activeGuest(now, signals.cameraId);
      const bool personMatches = guest && (guest->personId == 0 ||
                                           guest->personId == signals.personId);
      if (guest && personMatches) {
        context.expectedGuest = true;
        if (guest->oneTime) {
          checkpoint.guestId = guest->id;
          checkpoint.guestOneTime = true;
        }
      }
    }
    const GuardDanger policyDanger = guard_policy::evaluate(context);
    checkpoint.policyDanger = guard_policy::dangerToString(policyDanger);
    checkpoint.visitCount = context.visitCount;
    checkpoint.expectedGuest = context.expectedGuest;
    checkpoint.hardFloor = guard_policy::dangerRank(policyDanger) >=
                           guard_policy::dangerRank(GuardDanger::High);
    hardFloor = checkpoint.hardFloor;
    danger = policyDanger;
  }
  else {
    danger = guard_policy::dangerFromString(checkpoint.policyDanger);
  }

  if (state.stage < kStageIncident) {
    const GuardIncidentPhaseResult phase =
        co_await repository_.commitIncidentPhase(
            {.incident = {.cameraId = signals.cameraId,
                          .cameraName = signals.cameraName,
                          .rule = signals.rule,
                          .danger = guard_policy::dangerToString(danger),
                          .severity = signals.severity,
                          .personId = signals.personId,
                          .identity = signals.hasKnown ? "known" : "unknown",
                          .eventId = eventId,
                          .eventJson = json_util::toString(event),
                          .createdAt = now},
             .consumeGuestId = checkpoint.guestId,
             .consumeGuestAt = now,
             .advance = {.eventId = eventId,
                         .stage = kStageIncident,
                         .incidentId = 0,
                         .encounterId = state.encounterId,
                         .danger = checkpoint.danger,
                         .checkpoint =
                             json_util::toString(checkpointToJson(checkpoint)),
                         .at = now}});
    if (!phase.committed) {
      throw std::runtime_error("incident phase commit failed");
    }
    state.incidentId = phase.incidentId;
    state.stage = kStageIncident;
    failAt("after_incident");
  }

  if (state.stage < kStageEncounter) {
    const auto candidates =
        co_await repository_.openEncounters(now - config_.crossCameraWindowS);
    std::vector<GuardEncounterCandidate> openEncounters;
    openEncounters.reserve(candidates.size());
    for (const auto& encounter : candidates) {
      openEncounters.push_back({.id = encounter.id,
                                .personId = encounter.personId,
                                .signature = encounter.signature,
                                .lastCameraId = encounter.bestCameraId,
                                .lastSeen = encounter.lastSeen});
    }
    const auto matched = guard_policy::matchEncounter(
        {.personId = signals.personId,
         .signature = signals.signature,
         .cameraId = signals.cameraId,
         .now = now,
         .windowS = config_.crossCameraWindowS,
         .continuityWindowS = config_.continuityWindowS,
         .minSimilarity = config_.signatureMinSimilarity,
         .candidates = openEncounters});
    GuardEncounterPhaseInput phase;
    phase.advance = {.eventId = eventId,
                     .stage = kStageEncounter,
                     .incidentId = state.incidentId,
                     .encounterId = 0,
                     .danger = checkpoint.danger,
                     .checkpoint =
                         json_util::toString(checkpointToJson(checkpoint)),
                     .at = now};
    if (matched) {
      phase.encounterIdToTouch = *matched;
      phase.touchInput = {.id = *matched,
                          .bestCameraId = signals.cameraId,
                          .bestScore = signals.viewScore,
                          .lastSeen = now};
    }
    else if (signals.hasUnknown) {
      phase.create = true;
      phase.createInput = {.personId = signals.personId,
                           .signature = signals.signature,
                           .bestCameraId = signals.cameraId,
                           .bestScore = signals.viewScore,
                           .at = now};
    }
    if (signals.hasKnown && !signals.hasUnknown && signals.personId > 0) {
      phase.closeForPerson = true;
      phase.closePersonId = signals.personId;
      phase.closeAt = now;
    }
    const GuardEncounterPhaseResult encounterPhase =
        co_await repository_.commitEncounterPhase(phase);
    if (!encounterPhase.committed) {
      throw std::runtime_error("encounter phase commit failed");
    }
    state.encounterId = encounterPhase.encounterId;
    checkpoint.encounterChecks = encounterPhase.encounterChecks;
    for (const auto& encounter : encounterPhase.closed)
      publishEncounterClosed(encounter, now);
    state.stage = kStageEncounter;
    failAt("after_encounter");
  }

  if (state.stage < kStageDialogue) {
    const bool knownVisit = signals.hasKnown && !signals.hasUnknown &&
                            signals.personId > 0 && config_.greetKnown;
    const bool unknownVisit = signals.hasUnknown && state.encounterId > 0 &&
                              checkpoint.encounterChecks == 1;
    bool continuesDialogue = false;
    if (state.encounterId > 0) {
      const auto encounter =
          co_await repository_.findEncounter(state.encounterId);
      continuesDialogue = encounter.has_value() &&
                          !encounter->dialogueGoal.empty() &&
                          encounter->dialogueGoal != "done";
    }
    if (config_.greetEnabled && mode == GuardMode::Home &&
        (knownVisit || unknownVisit || continuesDialogue) && !hardFloor) {
      checkpoint.dialogue =
          co_await runDialogue({.signals = signals,
                                .knownVisit = knownVisit,
                                .unknownVisit = unknownVisit,
                                .incidentId = state.incidentId,
                                .encounterId = state.encounterId,
                                .now = now,
                                .danger = danger,
                                .correlationId = eventId});
      checkpoint.greetingStatus = checkpoint.dialogue.greetingStatus;
      checkpoint.greetingDetail = checkpoint.dialogue.greetingDetail;
      if (checkpoint.dialogue.resumable) {
        co_return ObservationResult{.completed = false,
                                    .retryAt = checkpoint.dialogue.retryAt};
      }
    }
    state.stage = kStageDialogue;
    failAt("after_dialogue");
    co_await advanceObservation({.eventId = eventId,
                                 .stage = state.stage,
                                 .incidentId = state.incidentId,
                                 .encounterId = state.encounterId,
                                 .checkpoint = checkpoint,
                                 .at = now});
  }

  if (state.stage < kStageAssessment) {
    if (dependencies_.assessment && signals.hasUnknown) {
      bool personHasUser = false;
      std::string personName;
      std::string personRole;
      std::string personObservation;
      std::vector<std::string> knownTags;
      if (signals.personId > 0 && dependencies_.identity) {
        const auto person = co_await BlockingTask<std::optional<PersonProfile>>(
            [this, personId = signals.personId]() {
              return dependencies_.identity->getPerson(personId);
            });
        if (person) {
          personHasUser = person->userId.has_value();
          personName = person->name;
          personRole = person->role;
          personObservation = person->observation;
          knownTags = person->tags;
        }
      }
      const auto assessStart = std::chrono::steady_clock::now();
      checkpoint.assessment = co_await dependencies_.assessment->assess(
          {.cameraId = signals.cameraId,
           .trackId = signals.trackId,
           .firstSeenMs = signals.firstSeenMs,
           .publishedAtMs = signals.publishedAtMs,
           .rule = signals.rule,
           .profile = config_.profile,
           .visitCount = checkpoint.visitCount,
           .checks = checkpoint.encounterChecks,
           .expectedGuest = checkpoint.expectedGuest,
           .danger = danger,
           .personHasUser = personHasUser,
           .personName = personName,
           .personRole = personRole,
           .personObservation = personObservation,
           .knownTags = knownTags,
           .greeted = checkpoint.dialogue.greeted,
           .greetingText = checkpoint.dialogue.greetingText,
           .personReply = checkpoint.dialogue.heardText,
           .replied = checkpoint.dialogue.replied,
           .replyText = checkpoint.dialogue.replyText,
            .dialogueTurns = checkpoint.dialogue.turns});
      assessMs = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - assessStart)
              .count());
      const bool soft = guard_policy::dangerRank(danger) <
                        guard_policy::dangerRank(GuardDanger::High);
      if (checkpoint.assessment.performed && checkpoint.assessment.veto &&
          config_.vetoScope == "soft_only" && soft)
        danger = GuardDanger::Low;
      if (checkpoint.assessment.performed) {
        const GuardRiskResult risk =
            guard_risk::mergeEvidence({.floor = danger,
                                       .threat = checkpoint.assessment.threat,
                                       .tags = checkpoint.assessment.tags,
                                       .hardFloor = hardFloor});
        danger = risk.danger;
        checkpoint.assessment.tags = risk.appliedTags;
      }
    }
    checkpoint.danger = guard_policy::dangerToString(danger);
    state.stage = kStageAssessment;
    failAt("after_assessment");
    co_await advanceObservation({.eventId = eventId,
                                 .stage = state.stage,
                                 .incidentId = state.incidentId,
                                 .encounterId = state.encounterId,
                                 .checkpoint = checkpoint,
                                 .at = now});
  }
  else {
    danger = guard_policy::dangerFromString(checkpoint.danger);
  }

  if (state.stage < kStageResolve) {
    bool observedOnly = false;
    if (config_.stagingEnabled && signals.hasUnknown && !hardFloor &&
        guard_policy::dangerRank(danger) <
            guard_policy::dangerRank(GuardDanger::High)) {
      observedOnly = checkpoint.encounterChecks < config_.loiterChecks;
      if (!observedOnly)
        danger = GuardDanger::High;
    }
    checkpoint.observedOnly = observedOnly;
    checkpoint.danger = guard_policy::dangerToString(danger);

    co_await repository_.updateIncidentDanger(state.incidentId,
                                               checkpoint.danger);

    if (checkpoint.observedOnly) {
      const int stagedRank = guard_policy::dangerRank(danger);
      const bool stagedLegacyWould = stagedRank >= config_.notifyLevel;
      std::vector<std::string> stagedKinds;
      if (stagedRank >= config_.notifyLevel)
        stagedKinds.push_back("notify");
      if (stagedRank >= config_.announceLevel)
        stagedKinds.push_back("announce");
      if (stagedRank >= config_.alarmLevel) {
        stagedKinds.push_back("alarm");
        stagedKinds.push_back("siren_arm");
      }
      const CollectionResult stagedCollected = co_await collectSignals(
          {.cameraId = signals.cameraId,
           .signature = signals.signature,
           .hasUnknown = signals.hasUnknown,
           .now = now});
      const HoldResult stagedHolds = co_await computeHolds(
          {.danger = danger,
           .legacyWouldNotify = stagedLegacyWould,
           .now = now});
      co_await journalDecision(
          {.eventId = eventId,
           .encounterId = state.encounterId,
           .incidentId = state.incidentId,
           .cameraId = signals.cameraId,
           .observationId = signals.observationId,
           .danger = danger,
           .hardFloor = checkpoint.hardFloor,
           .beliefScore = 0,
           .beliefSignals = {},
           .beliefThreshold = 0,
           .legacyWouldNotify = stagedLegacyWould,
           .beliefWouldNotify = false,
           .decisionMode = config_.decisionMode,
           .suppression = DecisionSuppression::Staging,
           .suppressedKinds = stagedKinds,
           .noveltyScore = stagedCollected.noveltyScore,
           .repeatVisits = stagedCollected.repeatVisits,
           .quietHold = stagedHolds.quiet,
           .budgetHold = stagedHolds.budget,
           .assessMs = 0,
           .at = now});
    }

    if (checkpoint.assessment.performed) {
      co_await repository_.insertAssessment(
          {.incidentId = state.incidentId,
           .cameraId = signals.cameraId,
           .eventId = eventId,
           .mode = checkpoint.assessment.mode,
           .caption = checkpoint.assessment.caption,
           .threat = checkpoint.assessment.threat,
           .veto = checkpoint.assessment.veto,
           .tags = tagsToJson(checkpoint.assessment.tags),
           .summary = checkpoint.assessment.summary,
           .createdAt = now});
      if (!checkpoint.assessment.tags.empty() && signals.personId > 0 &&
          dependencies_.identity) {
        co_await BlockingTask<bool>([this, personId = signals.personId,
                                     tags = checkpoint.assessment.tags]() {
          return dependencies_.identity->tagPerson({.personId = personId,
                                                    .tags = tags,
                                                    .source = "llm",
                                                    .observation = {}});
        });
      }
      int logIndex = 0;
      for (const auto& log : checkpoint.assessment.toolLogs) {
        co_await repository_.insertAction(
            {.incidentId = state.incidentId,
             .encounterId = state.encounterId,
             .cameraId = signals.cameraId,
             .personId = signals.personId,
             .commandId = eventId + ":agent:" + log.kind + ":" +
                          std::to_string(logIndex++),
             .kind = "agent_" + log.kind,
             .status = log.status,
             .detail = {},
             .createdAt = now});
      }
    }

    if (!hardFloor && checkpoint.dialogue.greeted &&
        !checkpoint.dialogue.replied && config_.greetReplyEnabled &&
        guard_policy::dangerRank(danger) <
            guard_policy::dangerRank(GuardDanger::High) &&
        (!checkpoint.dialogue.heardText.empty() ||
         checkpoint.dialogue.repairPending)) {
      std::vector<std::string> variants = config_.greetReplyTexts;
      if (variants.empty() && !config_.greetReplyText.empty())
        variants.push_back(config_.greetReplyText);
      std::string grounded;
      if (checkpoint.assessment.performed && checkpoint.assessment.valid &&
          !checkpoint.assessment.announceText.empty())
        grounded = checkpoint.assessment.announceText;
      else if (checkpoint.dialogue.repairPending &&
               !config_.greetRepairText.empty())
        grounded = config_.greetRepairText;
      else
        grounded = guard_dialogue::pickVaried(
            {.variants = variants,
             .seed = state.encounterId,
             .exclude = checkpoint.dialogue.replyText});
      grounded = sanitizeSpoken(grounded);
      if (!grounded.empty()) {
        const EffectResult reply =
            co_await performEffect({.kind = GuardActionKind::Reply,
                                    .danger = danger,
                                    .replyRequested = true,
                                    .cameraId = signals.cameraId,
                                    .incidentId = state.incidentId,
                                    .encounterId = state.encounterId,
                                    .personId = signals.personId,
                                    .now = now,
                                    .text = grounded,
                                    .lang = config_.greetReplyLang,
                                    .correlationId = eventId,
                                    .sequence = 100,
                                    .notifyContent = {}});
        if (reply.resumable) {
          co_return ObservationResult{.completed = false,
                                      .retryAt = reply.retryAt};
        }
        if (reply.indeterminate) {
          co_return ObservationResult{};
        }
        checkpoint.dialogue.replied = reply.accepted;
        checkpoint.dialogue.replyText =
            reply.accepted ? grounded : std::string{};
        std::string nextGoal(kGoalDone);
        if (reply.accepted && checkpoint.dialogue.listenAfterReply &&
            config_.greetListenSeconds > 0 && state.encounterId > 0) {
          const int seconds = std::clamp(config_.greetListenSeconds, 1, 10);
          co_await repository_.setDialogueGoal(
              {.encounterId = state.encounterId,
               .goal = std::string(kGoalAwaitHelp),
               .listeningUntil = now + seconds,
               .lastLine = grounded,
               .lastHeard = {}});
          failAt("before_offer_listen");
          const EffectResult heard =
              co_await performEffect({.kind = GuardActionKind::Listen,
                                      .danger = danger,
                                      .cameraId = signals.cameraId,
                                      .incidentId = state.incidentId,
                                      .encounterId = state.encounterId,
                                      .personId = signals.personId,
                                      .now = now,
                                      .lang = config_.greetLang,
                                      .seconds = seconds,
                                      .correlationId = eventId,
                                      .sequence = 101,
                                      .notifyContent = {}});
          failAt("after_offer_listen");
          if (heard.resumable) {
            co_return ObservationResult{.completed = false,
                                        .retryAt = heard.retryAt};
          }
          if (heard.indeterminate)
            co_return ObservationResult{};
          checkpoint.dialogue.heardText =
              heard.accepted ? heard.heard : std::string{};
          nextGoal = checkpoint.dialogue.heardText.empty()
                         ? std::string(kGoalDone)
                         : std::string(kGoalAwaitHelp);
        }
        if (state.encounterId > 0)
          co_await repository_.recordDialogue(
              {.encounterId = state.encounterId,
               .turnKey = eventId + ":offer",
               .goal = nextGoal,
               .listeningUntil = 0,
               .lastLine = grounded,
               .lastHeard = checkpoint.dialogue.heardText,
               .at = now});
      }
      else if (state.encounterId > 0) {
        co_await repository_.setDialogueGoal(
            {.encounterId = state.encounterId,
             .goal = std::string(kGoalDone),
             .listeningUntil = 0,
             .lastLine = checkpoint.dialogue.greetingText,
             .lastHeard = checkpoint.dialogue.heardText});
      }
    }

    if (state.encounterId > 0) {
      EncounterState encounterState = EncounterState::Verifying;
      std::string reason = "soft_verified";
      if (observedOnly) {
        encounterState = EncounterState::Observing;
        reason = "observed";
      }
      else if (guard_policy::dangerRank(danger) >=
               guard_policy::dangerRank(GuardDanger::High)) {
        encounterState = EncounterState::Escalating;
        reason = "escalated";
      }
      co_await repository_.transitionEncounter(
          {.encounterId = state.encounterId,
           .toState = encounterState,
           .reason = reason,
           .grade = guard_policy::dangerToString(danger),
           .at = now});
    }
    state.stage = kStageResolve;
    failAt("after_resolve");
    co_await advanceObservation({.eventId = eventId,
                                 .stage = state.stage,
                                 .incidentId = state.incidentId,
                                 .encounterId = state.encounterId,
                                 .checkpoint = checkpoint,
                                 .at = now});
  }
  else {
    danger = guard_policy::dangerFromString(checkpoint.danger);
  }

  if (checkpoint.observedOnly)
    co_return ObservationResult{};

  if (state.stage < kStageEffects && !checkpoint.effectsDenied &&
      !checkpoint.effectsStarted) {
    const int rank = guard_policy::dangerRank(danger);
    const int lowestLevel = std::min(
        {config_.notifyLevel, config_.announceLevel, config_.alarmLevel});
    const bool effectsRequested = rank >= lowestLevel;
    if (effectsRequested) {
      const bool allowed = state.encounterId > 0
                               ? co_await repository_.canActEncounter(
                                     {.id = state.encounterId,
                                      .now = now,
                                      .cooldownS = config_.actionCooldownS,
                                      .maxPerHour = config_.maxActionsPerHour})
                               : co_await repository_.canAct(
                                     {.cameraId = signals.cameraId,
                                      .personId = signals.personId,
                                      .cooldownS = config_.actionCooldownS,
                                      .now = now,
                                      .maxPerHour = config_.maxActionsPerHour});
      if (!allowed) {
        checkpoint.effectsDenied = true;
        co_await repository_.insertAction({.incidentId = state.incidentId,
                                           .encounterId = state.encounterId,
                                           .cameraId = signals.cameraId,
                                           .personId = signals.personId,
                                           .commandId = {},
                                           .kind = "notify",
                                           .status = "budget_denied",
                                           .detail = "cooldown_or_hourly_cap",
                                           .createdAt = now});
        state.stage = kStageEffects;
        co_await advanceObservation({.eventId = eventId,
                                     .stage = state.stage,
                                     .incidentId = state.incidentId,
                                     .encounterId = state.encounterId,
                                     .checkpoint = checkpoint,
                                     .at = now});
      }
      else {
        checkpoint.effectsStarted = true;
        co_await advanceObservation({.eventId = eventId,
                                     .stage = state.stage,
                                     .incidentId = state.incidentId,
                                     .encounterId = state.encounterId,
                                     .checkpoint = checkpoint,
                                     .at = now});
      }
    }
  }

  DecisionSuppression suppression = DecisionSuppression::LegacySilent;
  std::vector<std::string> beliefSignalNames;
  std::vector<std::string> suppressedKinds;
  bool notifySuppressed = false;
  bool announceSuppressed = false;
  bool alarmSuppressed = false;
  if (state.stage < kStageEffects || checkpoint.effectsDenied) {
    const int rank = guard_policy::dangerRank(danger);
    const bool legacyWould = rank >= config_.notifyLevel;
    const BeliefConfig beliefCfg = beliefConfig(signals.cameraId);
    const std::string healthStatus = cameraHealthStatus(signals.cameraId);
    const BeliefResult belief = guard_belief::evaluateBelief(
        {.scoreMedian = signals.scoreMedian,
         .scoreSamples = signals.scoreSamples,
         .dwellMs = signals.dwellMs,
         .zoneWindows = signals.zoneWindows,
         .trackWindows = signals.trackWindows,
         .zoneKind = signals.zoneKind,
         .trackAgeMs = signals.publishedAtMs - signals.firstSeenMs,
         .areaSpread = signals.areaSpread,
         .identity = signals.identityState,
         .identityAvailable = signals.identityAvailable,
         .healthDegraded = !healthStatus.empty() && healthStatus != "ok",
         .config = beliefCfg});
    const int threshold = guard_belief::beliefThreshold(danger, beliefCfg);
    const bool beliefWould = belief.score >= threshold;
    const bool enforce = config_.decisionMode == "enforce";
    bool threadRepeat = false;
    if (legacyWould && !checkpoint.effectsDenied && state.encounterId > 0) {
      const auto encounter =
          co_await repository_.findEncounter(state.encounterId);
      threadRepeat = encounter.has_value() &&
                     encounter->notifyCount > 0 &&
                     rank <= encounter->notifyHighestRank;
    }
    const bool notifyRequested = rank >= config_.notifyLevel;
    const bool announceRequested = rank >= config_.announceLevel;
    const bool alarmRequested = rank >= config_.alarmLevel;
    const bool beliefBlocks = enforce && !beliefWould;
    const auto scopeCovers = [this, hardFloor = checkpoint.hardFloor](
                                 GuardActionKind kind) {
      return guard_belief::beliefSuppressesKind(
          {.scope = config_.beliefGateScope,
           .kind = kind,
           .hardFloor = hardFloor});
    };
    notifySuppressed =
        notifyRequested &&
        (threadRepeat ||
         (beliefBlocks && scopeCovers(GuardActionKind::Notify)));
    announceSuppressed = announceRequested && beliefBlocks &&
                         scopeCovers(GuardActionKind::Announce);
    alarmSuppressed = alarmRequested && beliefBlocks &&
                      scopeCovers(GuardActionKind::Alarm);
    if (!legacyWould)
      suppression = DecisionSuppression::LegacySilent;
    else if (checkpoint.effectsDenied)
      suppression = DecisionSuppression::Budget;
    else if (threadRepeat)
      suppression = DecisionSuppression::ThreadSuppressed;
    else if (notifySuppressed)
      suppression = DecisionSuppression::BeliefGate;
    else
      suppression = DecisionSuppression::None;
    suppressedKinds.clear();
    if (threadRepeat && notifyRequested)
      suppressedKinds.push_back("notify");
    if (beliefBlocks) {
      if (notifyRequested && scopeCovers(GuardActionKind::Notify) &&
          !threadRepeat)
        suppressedKinds.push_back("notify");
      if (announceRequested && scopeCovers(GuardActionKind::Announce))
        suppressedKinds.push_back("announce");
      if (alarmRequested && scopeCovers(GuardActionKind::Alarm)) {
        suppressedKinds.push_back("alarm");
        suppressedKinds.push_back("siren_arm");
      }
    }
    beliefSignalNames.clear();
    for (const auto& signal : belief.signals)
      beliefSignalNames.push_back(beliefSignalToString(signal));
    bool journalFresh = false;
    if (!co_await repository_.decisionJournalExists(eventId)) {
      const CollectionResult collected = co_await collectSignals(
          {.cameraId = signals.cameraId,
           .signature = signals.signature,
           .hasUnknown = signals.hasUnknown,
           .now = now});
      const HoldResult holds = co_await computeHolds(
          {.danger = danger, .legacyWouldNotify = legacyWould, .now = now});
      journalFresh = co_await journalDecision(
          {.eventId = eventId,
           .encounterId = state.encounterId,
           .incidentId = state.incidentId,
           .cameraId = signals.cameraId,
           .observationId = signals.observationId,
           .danger = danger,
           .hardFloor = checkpoint.hardFloor,
           .beliefScore = belief.score,
           .beliefSignals = beliefSignalNames,
           .beliefThreshold = threshold,
           .legacyWouldNotify = legacyWould,
           .beliefWouldNotify = beliefWould,
           .decisionMode = config_.decisionMode,
           .suppression = suppression,
           .suppressedKinds = suppressedKinds,
           .noveltyScore = collected.noveltyScore,
           .repeatVisits = collected.repeatVisits,
           .quietHold = holds.quiet,
           .budgetHold = holds.budget,
           .assessMs = assessMs,
           .at = now});
    }
    if (journalFresh && state.stage < kStageEffects) {
      for (const auto& kind : suppressedKinds) {
        const bool threadRow =
            kind == "notify" && suppression == DecisionSuppression::ThreadSuppressed;
        co_await repository_.insertAction(
            {.incidentId = state.incidentId,
             .encounterId = state.encounterId,
             .cameraId = signals.cameraId,
             .personId = signals.personId,
             .commandId = {},
             .kind = kind,
             .status = threadRow ? "thread_suppressed" : "belief_suppressed",
             .detail = threadRow ? "same_tier_repeat" : "belief_below_threshold",
             .createdAt = now});
      }
      const bool anyEffectRuns =
          !checkpoint.effectsDenied &&
          ((notifyRequested && !notifySuppressed) ||
           (announceRequested && !announceSuppressed) ||
           (alarmRequested && !alarmSuppressed));
      if (!anyEffectRuns) {
        state.stage = kStageEffects;
        co_await advanceObservation({.eventId = eventId,
                                     .stage = state.stage,
                                     .incidentId = state.incidentId,
                                     .encounterId = state.encounterId,
                                     .checkpoint = checkpoint,
                                     .at = now});
      }
    }
  }

  if (state.stage < kStageEffects && !checkpoint.effectsDenied) {
    const int rank = guard_policy::dangerRank(danger);
    bool resumable = false;
    int64_t retryAt = 0;
    const auto note = [&resumable, &retryAt](const EffectResult& effect) {
      if (!effect.resumable)
        return;
      resumable = true;
      retryAt =
          retryAt == 0 ? effect.retryAt : std::min(retryAt, effect.retryAt);
    };
    if (rank >= config_.notifyLevel && !notifySuppressed) {
      const NotifyBody content = buildNotifyBody(
          {.cameraName = signals.cameraName,
           .rule = signals.rule,
           .danger = danger,
           .cameraId = signals.cameraId,
           .incidentId = state.incidentId,
           .encounterId = state.encounterId,
           .zoneKind = signals.zoneKind,
           .dwellMs = signals.dwellMs,
           .identity = signals.identityState,
           .beliefSignals = beliefSignalNames});
      note(co_await performEffect(
          {.kind = GuardActionKind::Notify,
           .danger = danger,
           .cameraId = signals.cameraId,
           .cameraName = signals.cameraName,
           .rule = signals.rule,
           .incidentId = state.incidentId,
           .encounterId = state.encounterId,
           .personId = signals.personId,
           .now = now,
           .correlationId = eventId,
           .sequence = 1,
           .notifyContent = {.title = content.title,
                             .body = content.body,
                             .data = content.data}}));
    }

    if (rank >= config_.announceLevel && !announceSuppressed) {
      const std::string text = sanitizeSpoken(
          config_.announceText.empty() ? std::string{"Atencion: zona vigilada."}
                                       : config_.announceText);
      if (text.empty()) {
        co_await repository_.insertAction({.incidentId = state.incidentId,
                                           .encounterId = state.encounterId,
                                           .cameraId = signals.cameraId,
                                           .personId = signals.personId,
                                           .commandId = {},
                                           .kind = "announce",
                                           .status = "denied",
                                           .detail = "line_gate",
                                           .createdAt = now});
      }
      else
        note(co_await performEffect({.kind = GuardActionKind::Announce,
                                     .danger = danger,
                                     .cameraId = signals.cameraId,
                                     .incidentId = state.incidentId,
                                     .encounterId = state.encounterId,
                                     .personId = signals.personId,
                                     .now = now,
                                     .text = text,
                                     .lang = config_.announceLang,
                                     .correlationId = eventId,
                                     .sequence = 2,
                                   .notifyContent = {}}));
    }

    if (rank >= config_.alarmLevel && !alarmSuppressed) {
      note(co_await performEffect({.kind = GuardActionKind::Alarm,
                                   .danger = danger,
                                   .cameraId = signals.cameraId,
                                   .incidentId = state.incidentId,
                                   .encounterId = state.encounterId,
                                   .personId = signals.personId,
                                   .now = now,
                                   .seconds = config_.alarmSeconds,
                                   .correlationId = eventId,
                                   .sequence = 3,
                                   .notifyContent = {}}));
      note(co_await performEffect({.kind = GuardActionKind::SirenArm,
                                   .danger = danger,
                                   .cameraId = signals.cameraId,
                                   .incidentId = state.incidentId,
                                   .encounterId = state.encounterId,
                                   .personId = signals.personId,
                                   .now = now,
                                   .correlationId = eventId,
                                   .sequence = 4,
                                   .notifyContent = {}}));
    }
    if (resumable) {
      co_return ObservationResult{.completed = false, .retryAt = retryAt};
    }
    state.stage = kStageEffects;
    failAt("after_effects");
    co_await advanceObservation({.eventId = eventId,
                                 .stage = state.stage,
                                 .incidentId = state.incidentId,
                                 .encounterId = state.encounterId,
                                 .checkpoint = checkpoint,
                                 .at = now});
  }

  if (state.stage < kStageEvidence) {
    co_await uploadEvidence({.event = event,
                             .incidentId = state.incidentId,
                             .encounterId = state.encounterId,
                             .cameraId = signals.cameraId,
                             .cameraName = signals.cameraName,
                             .rule = signals.rule,
                             .danger = danger,
                             .now = now});
    state.stage = kStageEvidence;
    failAt("after_evidence");
    co_await advanceObservation({.eventId = eventId,
                                 .stage = state.stage,
                                 .incidentId = state.incidentId,
                                 .encounterId = state.encounterId,
                                 .checkpoint = checkpoint,
                                 .at = now});
  }
  co_return ObservationResult{};
}

drogon::Task<GuardService::DialogueResult>
GuardService::runDialogue(const DialogueInput& input)
{
  DialogueResult result;
  if (!dependencies_.actions || input.encounterId <= 0)
    co_return result;

  const auto encounter = co_await repository_.findEncounter(input.encounterId);
  const int turns = encounter ? encounter->dialogueTurns : 0;
  const std::string goal = encounter ? encounter->dialogueGoal : std::string{};
  if (goal == kGoalDone)
    co_return result;
  const bool canSpeakAgain = turns < std::max(1, config_.maxDialogueTurns);

  int sequence = 0;

  if ((goal.empty() || goal == kGoalVerify) && !canSpeakAgain)
    co_return result;

  if ((goal == kGoalAwaitReply || goal == kGoalRepair) && !canSpeakAgain)
    co_return result;

  if (goal.empty() || goal == kGoalVerify) {
    std::string greeting;
    if (goal.empty()) {
      if (input.knownVisit) {
        std::string name;
        if (dependencies_.identity) {
          const auto person =
              co_await BlockingTask<std::optional<PersonProfile>>(
                  [this, personId = input.signals.personId]() {
                    return dependencies_.identity->getPerson(personId);
                  });
          if (person)
            name = person->name;
        }
        greeting = config_.greetKnownText.empty() ? config_.greetText
                                                  : config_.greetKnownText;
        const std::string placeholder = "{name}";
        if (const auto at = greeting.find(placeholder);
            at != std::string::npos) {
          greeting.replace(at, placeholder.size(), name);
        }
      }
      else {
        const std::vector<std::string> variants =
            config_.greetTexts.empty()
                ? std::vector<std::string>{config_.greetText}
                : config_.greetTexts;
        greeting = guard_dialogue::pickVaried(
            {.variants = variants,
             .seed = input.encounterId,
             .exclude = encounter ? encounter->lastLine : std::string{}});
      }
      greeting = sanitizeSpoken(greeting);
    }
    else {
      greeting = encounter ? encounter->lastLine : std::string{};
    }
    if (greeting.empty())
      co_return result;

    if (goal.empty()) {
      const EffectResult greet =
          co_await performEffect({.kind = GuardActionKind::Greet,
                                  .danger = input.danger,
                                  .greetingEnabled = true,
                                  .cameraId = input.signals.cameraId,
                                  .incidentId = input.incidentId,
                                  .encounterId = input.encounterId,
                                  .personId = input.signals.personId,
                                  .now = input.now,
                                  .text = greeting,
                                  .lang = config_.greetLang,
                                  .correlationId = input.correlationId,
                                  .sequence = 1,
                                  .notifyContent = {}});
      result.greetingText = greeting;
      result.greetingStatus =
          !greet.authorized ? "denied" : (greet.accepted ? "sent" : "failed");
      result.greetingDetail = greet.detail;
      result.greeted = greet.accepted;
      if (greet.resumable) {
        result.resumable = true;
        result.retryAt = greet.retryAt;
        co_return result;
      }
      if (!greet.accepted)
        co_return result;
      co_await repository_.transitionEncounter(
          {.encounterId = input.encounterId,
           .toState = EncounterState::Challenging,
           .reason = "challenge",
           .grade = guard_policy::dangerToString(input.danger),
           .at = input.now});
      co_await repository_.setDialogueGoal({.encounterId = input.encounterId,
                                            .goal = std::string(kGoalVerify),
                                            .listeningUntil = 0,
                                            .lastLine = greeting,
                                            .lastHeard = {}});
    }
    else {
      result.greeted = true;
      result.greetingText = greeting;
    }

    if (config_.greetListenSeconds > 0) {
      if (goal.empty())
        co_await repository_.transitionEncounter(
            {.encounterId = input.encounterId,
             .toState = EncounterState::Listening,
             .reason = "listen",
             .grade = guard_policy::dangerToString(input.danger),
             .at = input.now});
      const int seconds = std::clamp(config_.greetListenSeconds, 1, 10);
      co_await repository_.setDialogueGoal(
          {.encounterId = input.encounterId,
           .goal = std::string(kGoalVerify),
           .listeningUntil = input.now + seconds,
           .lastLine = greeting,
           .lastHeard = {}});
      failAt("before_challenge_listen");
      const EffectResult heard =
          co_await performEffect({.kind = GuardActionKind::Listen,
                                  .danger = input.danger,
                                  .cameraId = input.signals.cameraId,
                                  .incidentId = input.incidentId,
                                  .encounterId = input.encounterId,
                                  .personId = input.signals.personId,
                                  .now = input.now,
                                  .lang = config_.greetLang,
                                  .seconds = seconds,
                                  .correlationId = input.correlationId,
                                  .sequence = 2,
                                  .notifyContent = {}});
      failAt("after_challenge_listen");
      if (heard.resumable) {
        result.resumable = true;
        result.retryAt = heard.retryAt;
        co_return result;
      }
      result.listened = heard.executed;
      result.heardText = heard.heard;
      const bool unintelligible =
          !heard.accepted || heard.heard.empty() || !heard.speechDetected;
      result.repairPending = unintelligible && !config_.greetRepairText.empty();
      co_await repository_.recordDialogue(
          {.encounterId = input.encounterId,
           .turnKey = input.correlationId + ":challenge",
           .goal = result.repairPending ? std::string(kGoalRepair)
                                        : std::string(kGoalAwaitReply),
           .listeningUntil = 0,
           .lastLine = greeting,
           .lastHeard = result.heardText,
           .at = input.now});
    }
    else {
      co_await repository_.recordDialogue(
          {.encounterId = input.encounterId,
           .turnKey = input.correlationId + ":challenge",
           .goal = std::string(kGoalAwaitReply),
           .listeningUntil = 0,
           .lastLine = greeting,
           .lastHeard = {},
           .at = input.now});
    }
    result.turns = turns + 1;
    result.listenAfterReply = config_.greetListenSeconds > 0;
    co_await repository_.transitionEncounter(
        {.encounterId = input.encounterId,
         .toState = EncounterState::Interpreting,
         .reason = "interpret",
         .grade = guard_policy::dangerToString(input.danger),
         .at = input.now});
    co_return result;
  }

  if (goal == kGoalAwaitReply || goal == kGoalRepair) {
    result.greeted = true;
    result.greetingText = encounter ? encounter->lastLine : std::string{};
    result.heardText = encounter ? encounter->lastHeard : std::string{};
    result.listened = !result.heardText.empty();
    result.repairPending =
        goal == kGoalRepair && !config_.greetRepairText.empty();
    result.listenAfterReply = config_.greetListenSeconds > 0;
    result.turns = turns + 1;
    co_return result;
  }

  const bool heardAnswer = encounter && !encounter->lastHeard.empty() &&
                           encounter->lastHeardAt <= input.now;
  if (!heardAnswer && encounter && encounter->listeningUntil > input.now)
    co_return result;
  result.greeted = true;
  result.greetingText = encounter ? encounter->lastLine : std::string{};
  result.heardText = encounter ? encounter->lastHeard : std::string{};
  result.replied = true;
  result.turns = turns + 1;
  co_await repository_.recordDialogue(
      {.encounterId = input.encounterId,
       .turnKey = input.correlationId + ":answer",
       .goal = std::string(kGoalDone),
       .listeningUntil = 0,
       .lastLine = result.greetingText,
       .lastHeard = result.heardText,
       .at = input.now});
  co_return result;
}

drogon::Task<void> GuardService::uploadEvidence(const EvidenceInput& input)
{
  if (!storage_.isConfigured())
    co_return;
  Json::Value stored = input.event;
  for (auto& object : stored["objects"])
    object.removeMember("signature");
  Json::Value evidence;
  evidence["incidentId"] = Json::Int64(input.incidentId);
  evidence["encounterId"] = Json::Int64(input.encounterId);
  evidence["cameraId"] = Json::Int64(input.cameraId);
  evidence["cameraName"] = input.cameraName;
  evidence["rule"] = input.rule;
  evidence["danger"] = guardDangerToString(input.danger);
  evidence["event"] = stored;
  evidence["createdAt"] = Json::Int64(input.now);

  const bool serious =
      guardDangerRank(input.danger) >= guardDangerRank(GuardDanger::High);
  const std::string retentionClass =
      serious ? "extended"
              : (input.danger == GuardDanger::Medium ? "standard" : "short");
  const int64_t retentionS =
      serious ? 30LL * 24 * 3600
              : (input.danger == GuardDanger::Medium ? 7LL * 24 * 3600
                                                     : 24LL * 3600);
  const std::string objectKey = "guard/incidents/" +
                                std::to_string(input.cameraId) + "/" +
                                std::to_string(input.incidentId) + ".json";
  try {
    co_await storage_.put({.objectKey = objectKey,
                           .body = json_util::toString(evidence),
                           .contentType = "application/json"});
    co_await repository_.insertEvidence({.incidentId = input.incidentId,
                                         .encounterId = input.encounterId,
                                         .cameraId = input.cameraId,
                                         .objectKey = objectKey,
                                         .contentType = "application/json",
                                         .retentionClass = retentionClass,
                                         .createdAt = input.now,
                                         .expiresAt = input.now + retentionS});
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard evidence upload failed: " << error.what();
  }
  catch (...) {
    LOG_WARN << "Guard evidence upload failed with unknown error";
  }
  co_return;
}

drogon::Task<void> GuardService::runRetentionSweep()
{
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (config_.journalRetentionDays > 0) {
    const int64_t removed = co_await repository_.purgeDecisions(
        now - static_cast<int64_t>(config_.journalRetentionDays) * 86400);
    if (removed > 0)
      LOG_INFO << "Guard retention: purged " << removed
               << " decision journal row(s)";
  }
  if (!storage_.isConfigured()) {
    LOG_INFO << "Guard retention: object storage not configured; skipped";
    co_return;
  }
  int64_t removed = 0;
  for (int batch = 0; batch < 50; ++batch) {
    const auto expired = co_await repository_.expiredEvidence(now);
    if (expired.empty())
      break;
    for (const auto& row : expired) {
      try {
        co_await storage_.remove(row.objectKey);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Guard evidence removal failed: " << error.what();
        continue;
      }
      catch (...) {
        LOG_WARN << "Guard evidence removal failed with unknown error";
        continue;
      }
      co_await repository_.markEvidenceDeleted(row.id, now);
      ++removed;
    }
    if (expired.size() < 200)
      break;
  }
  if (removed > 0)
    LOG_INFO << "Guard retention: removed " << removed
             << " expired evidence object(s)";
  co_return;
}

void GuardService::scheduleRetentionSweep()
{
  const auto sweepOnce = [this, lifecycle = lifecycle_]()
                             -> drogon::Task<void> {
    const LifecycleGuard guard(lifecycle);
    if (!guard.alive())
      co_return;
    try {
      co_await runRetentionSweep();
    }
    catch (const std::exception& error) {
      LOG_WARN << "Guard service: retention sweep failed: "
               << error.what();
    }
    catch (...) {
      LOG_WARN << "Guard service: retention sweep failed with unknown error";
    }
    co_return;
  };
  trackTimer(drogon::app().getLoop()->runAfter(30.0, [sweepOnce]() {
    drogon::async_run(sweepOnce);
  }));
  trackTimer(drogon::app().getLoop()->runEvery(24.0 * 3600.0, [sweepOnce]() {
    drogon::async_run(sweepOnce);
  }));
}

// Map a camera command outcome onto the durable guard intent lifecycle.
GuardIntentStatus cameraIntentStatus(const CameraCommandResult& ack)
{
  if (ack.succeeded())
    return ack.outcome == CameraCommandOutcome::DUPLICATE_SUCCEEDED
               ? GuardIntentStatus::DuplicateSucceeded
               : GuardIntentStatus::Succeeded;
  if (ack.inFlight())
    return GuardIntentStatus::InFlight;
  if (ack.indeterminate())
    return GuardIntentStatus::Indeterminate;
  if (ack.conflict())
    return GuardIntentStatus::Conflict;
  if (ack.rejected())
    return GuardIntentStatus::Rejected;
  if (ack.retryable())
    return GuardIntentStatus::RetryableFailed;
  return GuardIntentStatus::Indeterminate;
}

drogon::Task<GuardService::EffectResult>
GuardService::performEffect(const EffectInput& input)
{
  EffectResult result;
  const std::string requestedId =
      makeCommandId({.correlationId = input.correlationId,
                     .kind = input.kind,
                     .sequence = input.sequence,
                     .cameraId = input.cameraId,
                     .now = input.now});
  const auto intent = co_await repository_.planIntent(
      {.commandId = requestedId,
       .encounterId = input.encounterId,
       .incidentId = input.incidentId,
       .cameraId = input.cameraId,
       .personId = input.personId,
       .kind = guardActionKindToString(input.kind),
       .payload = {},
       .at = input.now});
  if (!intent) {
    result.detail = "intent_unavailable";
    result.status = GuardIntentStatus::Rejected;
    co_return result;
  }
  const std::string commandId = intent->commandId;

  if (guardIntentStatusIsTerminal(intent->status)) {
    result.status = intent->status;
    const Json::Value response = json_util::fromString(intent->response);
    if (response.isObject()) {
      result.authorized = response.get("authorized", false).asBool();
      result.accepted = response.get("accepted", false).asBool();
      result.pending = response.get("pending", false).asBool();
      result.indeterminate = response.get("indeterminate", false).asBool();
      result.executed = response.get("executed", false).asBool();
      result.speechDetected = response.get("speechDetected", false).asBool();
      result.heard = response.get("heard", "").asString();
      result.detail = response.get("detail", "").asString();
    }
    if (result.detail.empty())
      result.detail = !intent->detail.empty()
                          ? intent->detail
                          : guardIntentStatusToString(intent->status);
    co_return result;
  }

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const int64_t nowMs = nowMillis();
  if ((intent->status == GuardIntentStatus::InFlight ||
       intent->status == GuardIntentStatus::RetryableFailed) &&
      intent->nextAttemptAt > nowMs) {
    result.executed = true;
    result.resumable = true;
    result.pending = true;
    result.status = intent->status;
    result.retryAt = intent->nextAttemptAt;
    result.detail =
        intent->detail.empty() ? "resumable_backoff" : intent->detail;
    co_return result;
  }

  failAt("after_intent");
  const GuardActionDecision decision =
      authorizer_.authorize({.kind = input.kind,
                             .danger = input.danger,
                             .greetingEnabled = input.greetingEnabled,
                             .replyRequested = input.replyRequested});
  result.authorized = decision.authorized;
  result.detail = decision.reason;
  if (result.authorized && !dependencies_.actions) {
    result.authorized = false;
    result.detail = "camera_unavailable";
  }

  const bool budgeted = input.kind == GuardActionKind::Notify ||
                        input.kind == GuardActionKind::Announce ||
                        input.kind == GuardActionKind::Alarm ||
                        input.kind == GuardActionKind::SirenArm;
  if (result.authorized && budgeted && config_.maxActionsPerHour > 0) {
    const int64_t used =
        co_await repository_.effectsSince(input.cameraId, input.now - 3600);
    if (used >= config_.maxActionsPerHour) {
      result.authorized = false;
      result.detail = "hourly_cap";
    }
  }

  const std::string effectKind = guardActionKindToString(input.kind);
  const auto applyCamera = [&result](const CameraCommandResult& ack) {
    result.status = cameraIntentStatus(ack);
    result.accepted = result.status == GuardIntentStatus::Succeeded ||
                      result.status == GuardIntentStatus::DuplicateSucceeded;
    result.pending = guardIntentStatusIsResumable(result.status);
    result.indeterminate = result.status == GuardIntentStatus::Indeterminate;
    result.detail = ack.transportOk() ? (ack.detail.empty() ? "ok" : ack.detail)
                                      : ack.status.error_message();
  };

  if (result.authorized) {
    result.executed = true;
    failAt("before_effect:" + effectKind);
    switch (input.kind) {
      case GuardActionKind::Notify: {
        const EffectStatus sent =
            co_await notify({.commandId = commandId,
                             .eventId = input.correlationId,
                             .payload = intent->payload,
                             .title = input.notifyContent.title,
                             .body = input.notifyContent.body,
                             .data = input.notifyContent.data,
                             .cameraId = input.cameraId,
                             .cameraName = input.cameraName,
                             .rule = input.rule,
                             .danger = input.danger,
                             .incidentId = input.incidentId,
                             .encounterId = input.encounterId,
                             .now = input.now});
        switch (sent) {
          case EffectStatus::Succeeded:
            result.status = GuardIntentStatus::Succeeded;
            result.accepted = true;
            result.detail = "notified";
            break;
          case EffectStatus::Rejected:
            result.status = GuardIntentStatus::Rejected;
            result.detail = "notification_rejected";
            break;
          case EffectStatus::Indeterminate:
            result.status = GuardIntentStatus::Indeterminate;
            result.indeterminate = true;
            result.detail = "notification_indeterminate";
            break;
          case EffectStatus::InFlight:
            result.status = GuardIntentStatus::InFlight;
            result.pending = true;
            result.detail = "notification_in_flight";
            break;
          case EffectStatus::RetryableFailed:
            result.status = GuardIntentStatus::RetryableFailed;
            result.pending = true;
            result.detail = "notification_failed";
            break;
        }
        break;
      }
      case GuardActionKind::Greet:
      case GuardActionKind::Reply:
      case GuardActionKind::Announce: {
        const auto ack = co_await BlockingTask<CameraCommandResult>(
            [this, cameraId = input.cameraId, text = input.text,
             lang = input.lang, commandId, encounterId = input.encounterId,
             expiresAt = input.now + 60]() {
              return dependencies_.actions->announce(
                  {.cameraId = cameraId,
                   .text = text,
                   .lang = lang,
                   .commandId = commandId,
                   .encounterId = encounterId,
                   .expiresAt = expiresAt});
            });
        applyCamera(ack);
        break;
      }
      case GuardActionKind::Listen: {
        const int seconds = std::clamp(input.seconds, 1, 10);
        const auto heard = co_await BlockingTask<CameraCommandResult>(
            [this, cameraId = input.cameraId, seconds, lang = input.lang,
             commandId, encounterId = input.encounterId]() {
              return dependencies_.actions->listen(
                  {.cameraId = cameraId,
                   .seconds = seconds,
                   .lang = lang,
                   .commandId = commandId,
                   .encounterId = encounterId});
            });
        applyCamera(heard);
        if (heard.succeeded()) {
          result.speechDetected = heard.speechDetected;
          result.heard = heard.text;
          if (result.heard.size() > 300)
            result.heard.resize(300);
        }
        break;
      }
      case GuardActionKind::Alarm: {
        const int seconds = std::clamp(input.seconds, 1, 10);
        const auto ack = co_await BlockingTask<CameraCommandResult>(
            [this, cameraId = input.cameraId, seconds, commandId,
             encounterId = input.encounterId, expiresAt = input.now + 60]() {
              return dependencies_.actions->alarm({.cameraId = cameraId,
                                                   .seconds = seconds,
                                                   .commandId = commandId,
                                                   .encounterId = encounterId,
                                                   .expiresAt = expiresAt});
            });
        applyCamera(ack);
        break;
      }
      case GuardActionKind::SirenArm:
      case GuardActionKind::SirenDisarm: {
        const bool enabled = input.kind == GuardActionKind::SirenArm;
        const int leaseSeconds = enabled ? config_.sirenSeconds : 0;
        const int64_t expiresAt = enabled ? input.now + 60 : 0;
        const auto ack = co_await BlockingTask<CameraCommandResult>(
            [this, cameraId = input.cameraId, enabled, commandId,
             encounterId = input.encounterId, expiresAt, leaseSeconds]() {
              return dependencies_.actions->setSiren(
                  {.cameraId = cameraId,
                   .enabled = enabled,
                   .commandId = commandId,
                   .encounterId = encounterId,
                   .expiresAt = expiresAt,
                   .leaseSeconds = leaseSeconds});
            });
        applyCamera(ack);
        if (enabled && result.accepted)
          scheduleSirenDisarm(input);
        break;
      }
    }
  }
  else {
    result.status = GuardIntentStatus::Rejected;
  }

  if (guardIntentStatusIsResumable(result.status)) {
    result.resumable = true;
    result.pending = true;
  }
  if (result.status == GuardIntentStatus::InFlight ||
      result.status == GuardIntentStatus::RetryableFailed)
    result.retryAt = retryBackoffAt(intent->attempts, nowMs);

  Json::Value response(Json::objectValue);
  response["authorized"] = result.authorized;
  response["accepted"] = result.accepted;
  response["pending"] = result.pending;
  response["indeterminate"] = result.indeterminate;
  response["executed"] = result.executed;
  response["detail"] = result.detail;
  response["speechDetected"] = result.speechDetected;
  response["heard"] = result.heard;
  co_await repository_.insertAction(
      {.incidentId = input.incidentId,
       .encounterId = input.encounterId,
       .cameraId = input.cameraId,
       .personId = input.personId,
       .commandId = commandId,
       .kind = effectKind,
       .status = guardIntentStatusToString(result.status),
       .detail = input.kind == GuardActionKind::Listen
                     ? (guardDangerRank(input.danger) >=
                                guardDangerRank(GuardDanger::Medium)
                            ? result.heard
                            : std::string{})
                     : result.detail,
       .createdAt = input.now});
  failAt("after_effect_rpc");
  failAt("after_effect_rpc:" + effectKind);
  if (!co_await repository_.updateOutbox({.commandId = commandId,
                                          .status = result.status,
                                          .detail = result.detail,
                                          .response =
                                              json_util::toString(response),
                                          .nextAttemptAt = result.retryAt,
                                          .at = input.now}))
    throw std::runtime_error("outbox settle failed");
  failAt("after_effect_settle");
  failAt("after_effect_settle:" + effectKind);
  co_return result;
}

void GuardService::scheduleSirenDisarm(const EffectInput& input)
{
  if (config_.sirenSeconds <= 0)
    return;
  EffectInput disarm = input;
  disarm.kind = GuardActionKind::SirenDisarm;
  disarm.text.clear();
  disarm.seconds = 0;
  trackTimer(drogon::app().getLoop()->runAfter(
      static_cast<double>(config_.sirenSeconds),
      [this, disarm = std::move(disarm), lifecycle = lifecycle_]() {
        drogon::async_run(
            [this, disarm, lifecycle]() -> drogon::Task<void> {
              const LifecycleGuard guard(lifecycle);
              if (!guard.alive())
                co_return;
              try {
                co_await performEffect(disarm);
              }
              catch (const std::exception& error) {
                LOG_WARN << "Guard service: siren disarm failed: "
                         << error.what();
              }
              catch (...) {
                LOG_WARN << "Guard service: siren disarm failed with unknown "
                            "error";
              }
              co_return;
            });
      }));
}

drogon::Task<GuardService::EffectStatus>
GuardService::notify(const NotifyInput& input)
{
  if (!dependencies_.notifications) {
    LOG_WARN << "Guard service: notification SDK unavailable";
    co_return EffectStatus::RetryableFailed;
  }

  std::vector<int64_t> userIds;
  std::string title;
  std::string body;
  Json::Value data;
  if (!input.payload.empty()) {
    const Json::Value stored = json_util::fromString(input.payload);
    if (stored.isObject()) {
      for (const auto& userId : stored["userIds"])
        userIds.push_back(userId.asInt64());
      title = stored.get("title", "").asString();
      body = stored.get("body", "").asString();
      data = stored["data"];
    }
    if (userIds.empty()) {
      LOG_ERROR << "Guard service: persisted notification payload unusable";
      co_return EffectStatus::RetryableFailed;
    }
  }

  if (userIds.empty()) {
    if (!dependencies_.identity) {
      LOG_WARN << "Guard service: identity SDK unavailable";
      co_return EffectStatus::RetryableFailed;
    }
    const auto resolved =
        co_await BlockingTask<std::optional<std::vector<int64_t>>>(
            [this]() { return dependencies_.identity->listNotifiableUsers(); });
    if (!resolved || resolved->empty()) {
      LOG_WARN << "Guard service: no notifiable users; notification dropped";
      co_return EffectStatus::RetryableFailed;
    }
    userIds = *resolved;
    if (!input.body.empty()) {
      title = input.title;
      body = input.body;
      data = input.data;
    }
    else {
      title = (input.cameraName.empty() ? "Camera" : input.cameraName) + ": " +
              input.rule;
      body = "Danger " + guardDangerToString(input.danger) + "; detected " +
             input.rule;
      data["cameraId"] = Json::Int64(input.cameraId);
      data["rule"] = input.rule;
      data["danger"] = guardDangerToString(input.danger);
      data["incidentId"] = Json::Int64(input.incidentId);
      data["encounterId"] = Json::Int64(input.encounterId);
    }

    Json::Value payload(Json::objectValue);
    Json::Value storedIds(Json::arrayValue);
    for (const int64_t userId : userIds)
      storedIds.append(Json::Int64(userId));
    payload["userIds"] = std::move(storedIds);
    payload["type"] = "camera";
    payload["title"] = title;
    payload["body"] = body;
    payload["data"] = data;
    bool persisted = false;
    if (config_.failPoint && config_.failPoint("notify_persist_fail"))
      persisted = false;
    else
      persisted = co_await repository_.setOutboxPayload(
          {.commandId = input.commandId,
           .payload = json_util::toString(payload),
           .at = input.now});
    if (!persisted) {
      LOG_ERROR << "Guard service: notification payload persistence failed";
      co_return EffectStatus::RetryableFailed;
    }
  }

  argus::notification::v1::CreateNotificationsRequest request;
  for (const int64_t userId : userIds)
    request.add_user_ids(userId);
  request.set_command_id(input.commandId);
  request.set_type("camera");
  request.set_title(title);
  request.set_body(body);
  request.set_data(json_util::toString(data));

  // Counted before the RPC so a crash or hang during the call still flags
  // the row as ambiguous. Attempts that never reach the service are counted
  // too; that is the conservative watch metric.
  if (!input.eventId.empty())
    co_await repository_.bumpDispatchAttempts(input.eventId);

  const auto created =
      co_await BlockingTask<NotificationCreateResult>([this, request]() {
        return dependencies_.notifications
            ->createNotifications(request, {.userId = 0,
                                            .role = "system",
                                            .device = "argus-guard"});
      });
  if (created.outcome == NotificationRpcOutcome::Conflict) {
    LOG_ERROR << "Guard service: notification command conflict for "
              << input.commandId;
    co_return EffectStatus::Rejected;
  }
  if (created.outcome == NotificationRpcOutcome::Rejected) {
    LOG_ERROR << "Guard service: notification rejected: "
              << created.status.error_message() << " for " << input.commandId;
    co_return EffectStatus::Rejected;
  }
  if (created.outcome == NotificationRpcOutcome::Unavailable) {
    LOG_WARN << "Guard service: notification service unavailable (camera "
             << input.cameraId << ")";
    co_return EffectStatus::RetryableFailed;
  }
  if (created.created != static_cast<int32_t>(userIds.size())) {
    LOG_WARN << "Guard service: notification batch incomplete ("
             << created.created << " of " << userIds.size() << "); will retry";
    co_return EffectStatus::RetryableFailed;
  }
  LOG_INFO << "Guard service: notified " << created.created << " user(s)"
           << (created.duplicate ? " (replayed)" : "") << " for camera "
           << input.cameraId << " (danger " << guardDangerToString(input.danger)
           << ")";
  if (!input.eventId.empty())
    // Flip and thread slot commit atomically; a crash between them rolls
    // both back, and a retry heals a previously diverged slot.
    co_await repository_.recordNotificationDispatch(
        {.eventId = input.eventId,
         .encounterId = input.encounterId,
         .commandId = input.commandId,
         .rank = guard_policy::dangerRank(input.danger),
         .failPoint = config_.failPoint});
  co_return EffectStatus::Succeeded;
}
