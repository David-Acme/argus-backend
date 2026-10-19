#include "guard-service.hxx"

#include "guard-assessment.hxx"
#include "guard-belief.hxx"
#include "guard-dialogue.hxx"
#include "guard-risk.hxx"

#include <algorithm>
#include <array>
#include <auth/role-access.hxx>
#include <camera/camera-action-client.hxx>
#include <camera/zone-type.hxx>
#include <feature/guard/vocabulary/decision-mode.hxx>
#include <map>
#include <ranges>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <drogon/drogon.h>
#include <exception>
#include <identity/identity-client.hxx>
#include <notification/notification-client.hxx>
#include <feature/guard/guard-tamper-policy.hxx>
#include <feature/guard/repositories/maintenance/maintenance-repository.hxx>
#include <feature/guard/services/maintenance-service.hxx>
#include <feature/guard/services/targeted-notice.hxx>
#include <storage/s3-storage-service.hxx>
#include <sync/stream-retention.hxx>
#include <text/json-util.hxx>
#include <runtime/blocking-task.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <string>
#include <system_error>
#include <trantor/utils/Logger.h>
#include <unordered_set>
#include <utility>
#include <vector>

constexpr double kRetryReconcileSeconds = 0.5;
constexpr int64_t kDestroyDeadlineSeconds = 10;
constexpr int64_t kThreadBusyRetryMs = 1000;

class ObservationRetryPump
    : public std::enable_shared_from_this<ObservationRetryPump>
{
public:
  void start(std::function<void()> tick)
  {
    tick_ = std::move(tick);
    stopping_.store(false, std::memory_order_release);
    if (started_.exchange(true))
      return;
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

  void stop() { stopping_.store(true, std::memory_order_release); }

  void tick()
  {
    if (stopping_.load(std::memory_order_acquire))
      return;
    if (tick_)
      tick_();
  }

private:
  std::function<void()> tick_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> started_{false};
};

namespace
{

Json::Value visitorToJson(const GuardVisitor& visitor)
{
  Json::Value json(Json::objectValue);
  json["name"] = visitor.name;
  json["category"] = visitor.category;
  json["visits"] = visitor.visits;
  Json::Value days(Json::arrayValue);
  for (const int day : visitor.weekdays)
    days.append(day);
  json["weekdays"] = days;
  json["usualHour"] = visitor.usualHour;
  json["companion"] = visitor.companion;
  return json;
}

GuardVisitor visitorFromJson(const Json::Value& json)
{
  GuardVisitor visitor;
  if (!json.isObject())
    return visitor;
  visitor.name = json.get("name", "").asString();
  visitor.category = json.get("category", "").asString();
  visitor.visits = json.get("visits", 0).asInt();
  for (const auto& day : json["weekdays"])
    if (day.isInt())
      visitor.weekdays.push_back(day.asInt());
  visitor.usualHour = json.get("usualHour", -1).asInt();
  visitor.companion = json.get("companion", false).asBool();
  return visitor;
}

GuardVisitor visitorOf(const PersonProfile& person, bool companion)
{
  GuardVisitor visitor;
  if (person.userId)
    return visitor;
  visitor.name = person.name;
  visitor.category = person.category;
  visitor.visits = person.visits;
  visitor.weekdays.assign(person.usualWeekdays.begin(), person.usualWeekdays.end());
  visitor.usualHour = person.usualHour.value_or(-1);
  visitor.companion = companion;
  return visitor;
}
std::string tagsToJson(const std::vector<std::string>& tags)
{
  Json::Value array(Json::arrayValue);
  for (const auto& tag : tags)
    array.append(tag);
  return json_util::toString(array);
}

int64_t effectExpiry()
{
  return static_cast<int64_t>(std::time(nullptr)) + 60;
}

int64_t nowMillis()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool alertZone(const GuardEventSignals& signals)
{
  return zoneTypeFromString(signals.zoneKind) == ZoneType::Alert ||
         signals.rule == "person_in_alert_zone";
}

bool protectionExpected(GuardMode mode)
{
  return mode == GuardMode::Away || mode == GuardMode::Armed ||
         mode == GuardMode::Night;
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

std::string makeCommandId(const CommandIdInput& input)
{
  const std::string kind = guardActionKindToString(input.kind);
  if (!input.correlationId.empty())
    return input.correlationId + ":" + kind + ":" +
           std::to_string(input.sequence);
  return std::to_string(input.cameraId) + ":" + std::to_string(input.now) +
         ":" + kind + ":" + std::to_string(gCommandSequence.fetch_add(1));
}

NoticeSubject subjectFor(const GuardEventSignals& signals)
{
  if (signals.strangerCount >= 2)
    return NoticeSubject::Several;
  if (signals.hasKnown && signals.hasUnknown)
    return NoticeSubject::Accompanied;
  if (!signals.identityAvailable ||
      signals.identityState == IdentityState::Unobservable)
    return NoticeSubject::Unobserved;
  return NoticeSubject::Stranger;
}

std::vector<GuardReason> reasonsFrom(const std::vector<std::string>& names)
{
  std::vector<GuardReason> reasons;
  reasons.reserve(names.size());
  for (const auto& name : names) {
    if (const auto reason = guardReasonFromString(name))
      reasons.push_back(*reason);
  }
  return reasons;
}

std::string reasonsJson(const std::vector<std::string>& names)
{
  Json::Value array(Json::arrayValue);
  for (const auto& name : names)
    array.append(name);
  return json_util::toString(array);
}

void addReason(std::vector<std::string>& reasons, GuardReason reason)
{
  const std::string name = guardReasonToString(reason);
  if (std::ranges::find(reasons, name) == reasons.end())
    reasons.push_back(name);
}

struct NoticeDataInput
{
  const GuardNotice& notice;
  int64_t environmentId{0};
  std::string digestDay;
  std::string rule;
  int64_t incidentId{0};
  int64_t encounterId{0};
  std::string zoneKind;
  std::string identityState;
  std::string phase;
};

Json::Value noticeData(const NoticeDataInput& input)
{
  const GuardNotice& notice = input.notice;
  Json::Value data(Json::objectValue);
  data["cameraId"] = static_cast<Json::Int64>(notice.cameraId);
  data["cameraName"] = notice.cameraName;
  data["environmentId"] = static_cast<Json::Int64>(input.environmentId);
  data["environmentName"] = notice.environmentName;
  data["rule"] = input.rule;
  data["danger"] = guardDangerToString(notice.danger);
  data["incidentId"] = static_cast<Json::Int64>(input.incidentId);
  data["encounterId"] = static_cast<Json::Int64>(input.encounterId);
  data["episodeId"] = static_cast<Json::Int64>(input.encounterId);
  data["zoneKind"] = input.zoneKind;
  data["dwellS"] = static_cast<Json::Int64>(std::max<int64_t>(0, notice.dwellS));
  data["identityState"] = input.identityState;
  data["kind"] = noticeKindToString(notice.kind);
  data["phase"] = input.phase;
  data["urgency"] = guard_copy::urgency(notice);
  data["action"] = noticeActionToString(notice.action);
  data["role"] = cameraRoleToString(notice.role);
  data["outdoor"] = notice.outdoor;
  data["subject"] = noticeSubjectToString(notice.subject);
  data["people"] = notice.people;
  Json::Value reasons(Json::arrayValue);
  for (const GuardReason reason : notice.reasons)
    reasons.append(guardReasonToString(reason));
  data["reasons"] = std::move(reasons);
  if (notice.visitor.present()) {
    Json::Value visitor = visitorToJson(notice.visitor);
    visitor["phraseEs"] = guard_copy::visitorSentence(notice.visitor, "es");
    visitor["phraseEn"] = guard_copy::visitorSentence(notice.visitor, "en");
    data["visitor"] = std::move(visitor);
  }
  switch (notice.kind) {
    case NoticeKind::Tamper:
      data["threadKey"] = "guard:tamper:" + std::to_string(notice.cameraId);
      break;
    case NoticeKind::Digest:
      data["threadKey"] = "guard:digest:" +
                          std::to_string(input.environmentId) + ":" +
                          input.digestDay;
      break;
    case NoticeKind::Episode:
    case NoticeKind::Escalation:
      data["threadKey"] =
          input.encounterId > 0
              ? "guard:episode:" + std::to_string(input.encounterId)
              : "guard:incident:" + std::to_string(input.incidentId);
      break;
    case NoticeKind::Panic:
      data["threadKey"] = "guard:panic:" + std::to_string(input.encounterId);
      break;
    case NoticeKind::Duress:
    case NoticeKind::SpokenDuress:
      data["threadKey"] = "guard:duress:" + std::to_string(input.encounterId);
      break;
    case NoticeKind::PrivacyCoverage:
      data["threadKey"] =
          "guard:privacy:" + std::to_string(input.environmentId);
      break;
    case NoticeKind::LensNotice:
      data["threadKey"] = "guard:lens:" + std::to_string(notice.cameraId);
      break;
    case NoticeKind::TamperAsk:
      data["threadKey"] = "guard:tamper:" + std::to_string(notice.cameraId);
      break;
    case NoticeKind::ViewCheck:
    case NoticeKind::MaintenanceDuress:
      data["threadKey"] = "guard:maintenance:" + std::to_string(notice.maintenanceId);
      break;
  }
  if (notice.kind == NoticeKind::Tamper || notice.kind == NoticeKind::TamperAsk ||
      notice.kind == NoticeKind::LensNotice || notice.kind == NoticeKind::ViewCheck) {
    data["tamperStatus"] = notice.tamperStatus;
    data["group"] = notice.lost.size() >= 2;
    Json::Value cameraIds(Json::arrayValue);
    for (const auto& line : notice.lost)
      cameraIds.append(static_cast<Json::Int64>(line.cameraId));
    if (notice.lost.empty() && notice.cameraId > 0)
      cameraIds.append(static_cast<Json::Int64>(notice.cameraId));
    data["cameraCount"] = static_cast<Json::Int64>(cameraIds.size());
    data["cameraIds"] = std::move(cameraIds);
    data["onsetAt"] = static_cast<Json::Int64>(notice.onsetAt);
    data["seenPersonName"] = notice.seenName;
    data["maintenance"] = notice.probableMaintenance ? "probable" : "";
    data["canClaim"] = notice.canClaim;
    data["askId"] = static_cast<Json::Int64>(notice.askId);
    data["maintenanceId"] = static_cast<Json::Int64>(notice.maintenanceId);
  }
  if (notice.kind == NoticeKind::PrivacyCoverage) {
    Json::Value masked(Json::arrayValue);
    for (const auto& camera : notice.masked) {
      Json::Value entry(Json::objectValue);
      entry["cameraId"] = static_cast<Json::Int64>(camera.cameraId);
      entry["name"] = camera.cameraName;
      masked.append(std::move(entry));
    }
    data["maskedCameras"] = std::move(masked);
  }
  return data;
}

std::string languageOf(const argus::identity::v1::GetUserResponse& response)
{
  return response.has_user() ? response.user().lang() : std::string{};
}

bool visitorPresent(const Json::Value& data)
{
  return data.isObject() && data["visitor"].isObject();
}

bool visitorNamed(const Json::Value& data)
{
  return visitorPresent(data) && !data["visitor"].get("name", "").asString().empty();
}

bool visitorRedactedFor(const Json::Value& data, UserRole role)
{
  if (!visitorPresent(data) || role == UserRole::Owner)
    return false;
  return role != UserRole::Guard || !visitorNamed(data);
}

constexpr std::string_view kDirectorySnapshotKey = "response_directory_snapshot";

std::string directorySnapshot(const std::vector<ResponseUser>& users)
{
  Json::Value array(Json::arrayValue);
  for (const auto& user : users) {
    Json::Value row(Json::objectValue);
    row["userId"] = static_cast<Json::Int64>(user.userId);
    row["role"] = userRoleToString(user.role);
    row["active"] = user.active;
    row["lang"] = user.lang;
    array.append(std::move(row));
  }
  return json_util::toString(array);
}

std::optional<std::vector<ResponseUser>> directoryFromSnapshot(const std::string& stored)
{
  const Json::Value array = json_util::fromString(stored);
  if (!array.isArray() || array.empty())
    return std::nullopt;
  std::vector<ResponseUser> users;
  users.reserve(array.size());
  for (const auto& row : array)
    users.push_back({.userId = row.get("userId", 0).asInt64(),
                     .role = userRoleFromString(row.get("role", "").asString()),
                     .active = row.get("active", false).asBool(),
                     .name = {},
                     .lang = row.get("lang", "").asString()});
  return users;
}

constexpr int64_t kLangCacheSeconds = 600;
constexpr int64_t kDayS = 86400;
constexpr int64_t kDigestLookbackS = 2 * kDayS;
}

struct GuardLifecycle
{
  std::atomic<bool> alive{true};
  std::atomic<int> active{0};
  std::mutex mutex;
  std::condition_variable idle;
};

class SweepFlag
{
public:
  explicit SweepFlag(std::atomic<bool>& flag) : flag_(flag) {}
  ~SweepFlag() { flag_.store(false, std::memory_order_release); }
  SweepFlag(const SweepFlag&) = delete;
  SweepFlag& operator=(const SweepFlag&) = delete;

private:
  std::atomic<bool>& flag_;
};

constexpr double kDeliveryKeepaliveS = 20.0;

class KeepaliveRelease
{
public:
  KeepaliveRelease(DeliveryKeepalive& keepalive, uint64_t token)
      : keepalive_(keepalive), token_(token)
  {
  }
  ~KeepaliveRelease() { keepalive_.release(token_); }
  KeepaliveRelease(const KeepaliveRelease&) = delete;
  KeepaliveRelease& operator=(const KeepaliveRelease&) = delete;

private:
  DeliveryKeepalive& keepalive_;
  uint64_t token_{0};
};

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
      std::scoped_lock lock(lifecycle_->mutex);
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
    std::scoped_lock lock(mutex_);
    owned_ = running_.insert(id_).second;
  }

  ~ExecutionLease()
  {
    if (!owned_)
      return;
    std::scoped_lock lock(mutex_);
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
    : dependencies_(std::move(dependencies)),
      config_(std::make_shared<const Config>(std::move(config))),
      lifecycle_(std::make_shared<GuardLifecycle>())
{
  retryPump_ = std::make_shared<ObservationRetryPump>();
}

std::shared_ptr<const GuardService::Config> GuardService::currentConfig() const
{
  std::scoped_lock lock(configMutex_);
  return config_;
}

void GuardService::refresh(const Config& fresh)
{
  {
    std::scoped_lock lock(configMutex_);
    auto next = std::make_shared<Config>(*config_);
    next->notifyLevel = fresh.notifyLevel;
    next->announceLevel = fresh.announceLevel;
    next->alarmLevel = fresh.alarmLevel;
    next->alarmSeconds = fresh.alarmSeconds;
    next->armSiren = fresh.armSiren;
    next->sirenSeconds = fresh.sirenSeconds;
    next->greetEnabled = fresh.greetEnabled;
    next->greetKnown = fresh.greetKnown;
    next->greetListenSeconds = fresh.greetListenSeconds;
    next->greetReplyEnabled = fresh.greetReplyEnabled;
    next->expectedGuestsEnabled = fresh.expectedGuestsEnabled;
    next->quietHoursEnabled = fresh.quietHoursEnabled;
    next->quietStartHour = fresh.quietStartHour;
    next->quietEndHour = fresh.quietEndHour;
    next->quietDailyBudget = fresh.quietDailyBudget;
    next->decisionMode = fresh.decisionMode;
    next->beliefGateScope = fresh.beliefGateScope;
    next->actionCooldownS = fresh.actionCooldownS;
    next->repeatWindowS = fresh.repeatWindowS;
    next->regroupWindowS = fresh.regroupWindowS;
    next->maxActionsPerHour = fresh.maxActionsPerHour;
    next->maxDialogueTurns = fresh.maxDialogueTurns;
    next->crossCameraWindowS = fresh.crossCameraWindowS;
    next->continuityWindowS = fresh.continuityWindowS;
    next->signatureMinSimilarity = fresh.signatureMinSimilarity;
    next->loiterChecks = fresh.loiterChecks;
    next->stagingEnabled = fresh.stagingEnabled;
    next->encounterTimeoutS = fresh.encounterTimeoutS;
    next->beliefRefreshS = fresh.beliefRefreshS;
    next->journalRetentionDays = fresh.journalRetentionDays;
    next->markedRetentionDays = fresh.markedRetentionDays;
    next->tamperSustainedS = fresh.tamperSustainedS;
    next->offlineSustainedS = fresh.offlineSustainedS;
    next->healthStaleS = fresh.healthStaleS;
    next->tamper = fresh.tamper;
    next->maintenance = fresh.maintenance;
    config_ = std::move(next);
  }
  std::scoped_lock lock(beliefMutex_);
  beliefCache_.clear();
}

GuardPosture GuardService::postureAt(const PostureInput& input)
{
  const auto at = static_cast<std::time_t>(input.now);
  std::tm local{};
  localtime_r(&at, &local);
  return guard_schedule::resolve(
      {.schedule = input.schedule, .manual = input.manual, .local = local});
}

drogon::Task<GuardEnvironmentScope>
GuardService::environmentScope(int64_t cameraId) const
{
  const auto config = currentConfig();
  try {
    if (const auto stored = co_await environmentRepository_.forCamera(cameraId))
      co_return *stored;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard service: environment read failed, using config: "
             << error.what();
  }
  co_return GuardEnvironmentScope{
      .environment = guard_schedule::environmentSeed(*config), .several = false};
}

void GuardService::rememberCameraName(int64_t cameraId, const std::string& name)
{
  if (cameraId <= 0 || name.empty())
    return;
  std::scoped_lock lock(healthMutex_);
  cameraNames_[cameraId] = name;
}

std::string GuardService::cameraName(int64_t cameraId) const
{
  std::scoped_lock lock(healthMutex_);
  const auto found = cameraNames_.find(cameraId);
  return found == cameraNames_.end() ? std::string{} : found->second;
}

GuardService::~GuardService()
{
  requestStop();
  if (dependencies_.bus) {
    if (durableSubscription_.has_value())
      dependencies_.bus->unsubscribe(*durableSubscription_);
    if (advisorySubscription_.has_value())
      dependencies_.bus->unsubscribe(*advisorySubscription_);
    if (healthSubscription_.has_value())
      dependencies_.bus->unsubscribe(*healthSubscription_);
    if (privacySubscription_.has_value())
      dependencies_.bus->unsubscribe(*privacySubscription_);
  }
  std::unique_lock lock(lifecycle_->mutex);
  const bool idle = lifecycle_->idle.wait_for(
      lock, std::chrono::seconds(kDestroyDeadlineSeconds), [lifecycle = lifecycle_] {
        return lifecycle->active.load(std::memory_order_acquire) == 0;
      });
  if (!idle)
    LOG_ERROR << "Guard service: destroyed with "
              << lifecycle_->active.load(std::memory_order_acquire)
              << " coroutine(s) still running";
}

void GuardService::requestStop()
{
  lifecycle_->alive.store(false, std::memory_order_release);
  if (retryPump_)
    retryPump_->stop();
  stopTimers();
}

bool GuardService::drained() const
{
  return lifecycle_->active.load(std::memory_order_acquire) == 0;
}

void GuardService::trackTimer(uint64_t id)
{
  std::scoped_lock lock(lifecycleMutex_);
  timerIds_.push_back(id);
}

void GuardService::stopTimers()
{
  std::vector<uint64_t> ids;
  {
    std::scoped_lock lock(lifecycleMutex_);
    ids = std::move(timerIds_);
  }
  if (!drogon::app().isRunning())
    return;
  for (const auto id : ids)
    drogon::app().getLoop()->invalidateTimer(id);
}

void GuardService::start()
{
  const auto config = currentConfig();
  if (dependencies_.bus)
    static_cast<void>(ensureGuardStream());
  if (!config->enabled)
    return;
  if (dependencies_.active && !dependencies_.active())
    return;
  if (subscribed_.load(std::memory_order_acquire))
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
           << (config->eventSubject.empty()
                   ? std::string(nats_subject::kCameraObjectDetected)
                   : config->eventSubject)
           << " (profile " << config->profile << ", durable "
           << config->consumerDurable << ")";
}

void GuardService::stop()
{
  if (dependencies_.bus) {
    if (durableSubscription_.has_value()) {
      dependencies_.bus->unsubscribe(*durableSubscription_);
      durableSubscription_.reset();
    }
    if (advisorySubscription_.has_value()) {
      dependencies_.bus->unsubscribe(*advisorySubscription_);
      advisorySubscription_.reset();
    }
    if (healthSubscription_.has_value()) {
      dependencies_.bus->unsubscribe(*healthSubscription_);
      healthSubscription_.reset();
    }
    if (privacySubscription_.has_value()) {
      dependencies_.bus->unsubscribe(*privacySubscription_);
      privacySubscription_.reset();
    }
  }
  if (drogon::app().isRunning()) {
    if (subscribeRetryTimer_.has_value())
      drogon::app().getLoop()->invalidateTimer(*subscribeRetryTimer_);
    for (const uint64_t id : heartbeatTimers_)
      drogon::app().getLoop()->invalidateTimer(id);
  }
  subscribeRetryTimer_.reset();
  heartbeatTimers_.clear();
  if (retryPump_)
    retryPump_->stop();
  subscribed_.store(false, std::memory_order_release);
  advisoriesSubscribed_ = false;
  healthSubscribed_ = false;
  privacySubscribed_ = false;
  heartbeatStarted_ = false;
  subscribeAttempts_ = 0;
}

bool GuardService::watching() const
{
  return subscribed_.load(std::memory_order_acquire);
}

void GuardService::applyModuleChange(const ModuleChange& change)
{
  if (change.id != role_access::kSurveillanceModule)
    return;
  drogon::app().getLoop()->queueInLoop([this, enabled = change.enabled]() {
    if (enabled)
      start();
    else
      stop();
  });
}

void GuardService::startHeartbeat()
{
  const auto config = currentConfig();
  if (heartbeatStarted_)
    return;
  heartbeatStarted_ = true;
  const uint64_t keepaliveTimer = drogon::app().getLoop()->runEvery(
      kDeliveryKeepaliveS, [keepalive = keepalive_, lifecycle = lifecycle_]() {
        const LifecycleGuard guard(lifecycle);
        if (guard.alive())
          keepalive->touch();
      });
  heartbeatTimers_.push_back(keepaliveTimer);
  trackTimer(keepaliveTimer);
  const uint64_t beatTimer = drogon::app().getLoop()->runEvery(
      static_cast<double>(std::max(1, config->heartbeatS)),
      [this, lifecycle = lifecycle_]() {
        const LifecycleGuard guard(lifecycle);
        if (!guard.alive())
          return;
        publishHeartbeat();
      });
  heartbeatTimers_.push_back(beatTimer);
  trackTimer(beatTimer);
}

drogon::Task<void> GuardService::reconcileObservations()
{
  const auto config = currentConfig();
  const LifecycleGuard guard(lifecycle_);
  if (!guard.alive())
    co_return;
  const int64_t nowMs = nowMillis();
  const auto due = co_await repository_.dueObservations(nowMs);
  for (const auto& row : due) {
    const bool claimed = co_await repository_.claimInboxRetry(
        {.eventId = row.eventId, .leaseUntil = nowMs + config->retryLeaseMs,
         .now = nowMs});
    if (!claimed)
      continue;
    enqueue({.payload = row.payload, .delivered = 0, .leased = true});
  }
  co_return;
}

bool GuardService::ensureGuardStream() const
{
  const auto config = currentConfig();
  return dependencies_.bus->ensureStream(
      {.name = config->guardStream,
       .subjects = {config->guardSubjectFilter.empty()
                        ? std::string(nats_subject::kGuardSubjectFilter)
                        : config->guardSubjectFilter},
       .maxAgeNs = stream_retention::kRetentionNs,
       .duplicatesNs = stream_retention::kDuplicatesNs});
}

bool GuardService::trySubscribe()
{
  const auto config = currentConfig();
  if (!ensureGuardStream())
    return false;
  const auto subscription = dependencies_.bus->subscribeDurable(
      {.stream = config->eventStream,
       .durable = config->consumerDurable,
       .subject = config->eventSubject.empty()
                      ? std::string(nats_subject::kCameraObjectDetected)
                      : config->eventSubject,
       .deliverAll = true,
       .maxDeliver = config->maxObservationAttempts,
       .maxAckPending = NatsBus::kDefaultMaxAckPending,
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
                accept({.payload = std::move(payload),
                        .ack = std::move(settlement.ack),
                        .nak = std::move(settlement.nak),
                        .term = std::move(settlement.term),
                        .inProgress = std::move(settlement.inProgress),
                        .delivered = delivered});
              });
        }});
  if (!subscription)
    return false;
  durableSubscription_ = *subscription;
  subscribed_.store(true, std::memory_order_release);
  subscribeAdvisories();
  subscribeHealth();
  subscribePrivacy();
  return true;
}

void GuardService::failAt(const std::string& name) const
{
  const auto config = currentConfig();
  if (config->failPoint && config->failPoint(name))
    throw std::runtime_error("failpoint: " + name);
}

std::string GuardService::sanitizeSpoken(const std::string& text) const
{
  const auto config = currentConfig();
  return guard_dialogue::sanitizeLine({.text = text,
                                       .maxWords = config->maxAnnounceWords,
                                       .privateTokens = {}});
}

void GuardService::subscribeAdvisories()
{
  const auto config = currentConfig();
  if (advisoriesSubscribed_ || !dependencies_.bus)
    return;
  const std::string subject =
      "$JS.EVENT.ADVISORY.CONSUMER.MAX_DELIVERIES.ARGUS_CAMERA." +
      config->consumerDurable;
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
        rememberCameraName(cameraId, health.get("cameraName", "").asString());
        ingestHealth({.cameraId = cameraId,
                      .status = health.get("status", "").asString(),
                      .atMs = nowMillis(),
                      .sudden = health.get("sudden", false).asBool(),
                      .link = health.get("link", "wifi").asString()});
      });
  if (subscription) {
    healthSubscription_ = *subscription;
    healthSubscribed_ = true;
  }
}

void GuardService::subscribePrivacy()
{
  if (privacySubscribed_ || !dependencies_.bus)
    return;
  const auto subscription = dependencies_.bus->subscribe(
      std::string(nats_subject::kCameraPrivacy),
      [this](std::string_view, std::string_view payload) {
        const Json::Value privacy =
            json_util::fromString(std::string(payload));
        if (!privacy.isObject())
          return;
        const int64_t cameraId = privacy.get("cameraId", 0).asInt64();
        if (cameraId <= 0)
          return;
        ingestPrivacy({.cameraId = cameraId,
                       .cameraName = privacy.get("cameraName", "").asString(),
                       .state = cameraPrivacyStateFromString(
                                    privacy.get("privacy", "").asString())
                                    .value_or(CameraPrivacyState::Unknown),
                       .obeyed = privacy.get("obeyed", false).asBool()});
      });
  if (subscription) {
    privacySubscription_ = *subscription;
    privacySubscribed_ = true;
  }
}

void GuardService::ingestHealth(const HealthSample& sample)
{
  if (sample.cameraId <= 0)
    return;
  std::scoped_lock lock(healthMutex_);
  CameraHealth& health = healthByCamera_[sample.cameraId];
  if (health.status == sample.status && health.lastSeenMs > 0) {
    health.lastSeenMs = sample.atMs;
    health.sudden = sample.sudden;
    health.link = sample.link;
    return;
  }
  health = {.status = sample.status,
            .firstSeenMs = sample.atMs,
            .lastSeenMs = sample.atMs,
            .sudden = sample.sudden,
            .link = sample.link};
}

std::string GuardService::healthStatusOf(int64_t cameraId) const
{
  return cameraHealthStatus(cameraId);
}

std::string GuardService::nameOf(int64_t cameraId) const
{
  return cameraName(cameraId);
}

void GuardService::attachMaintenance(const MaintenanceService* maintenance)
{
  maintenance_ = maintenance;
}

void GuardService::recordSightings(const SightingInput& input)
{
  constexpr int64_t kKeepMs = 120000;
  constexpr size_t kKeepCount = 32;
  const Json::Value& objects = input.event["objects"];
  if (!objects.isArray() || input.cameraId <= 0)
    return;
  const Json::Value& frame = input.event["frame"];
  const double frameArea = std::max(1.0, frame.get("width", 0).asDouble() * frame.get("height", 0).asDouble());
  std::scoped_lock lock(sightingMutex_);
  std::deque<KnownSighting>& sightings = knownSightings_[input.cameraId];
  for (const auto& object : objects) {
    if (object.get("class", "").asString() != "person" || object.get("identity", "").asString() != "known")
      continue;
    const int64_t personId = object.get("personId", 0).asInt64();
    if (personId <= 0)
      continue;
    sightings.push_back({.personId = personId,
                         .share = object["bbox"].get("w", 0.0).asDouble() *
                                  object["bbox"].get("h", 0.0).asDouble() / frameArea,
                         .atMs = input.atMs});
  }
  while (!sightings.empty() &&
         (sightings.size() > kKeepCount || input.atMs - sightings.front().atMs > kKeepMs))
    sightings.pop_front();
}

void GuardService::ingestPrivacy(const PrivacySample& sample)
{
  if (sample.cameraId <= 0)
    return;
  if (!sample.cameraName.empty())
    rememberCameraName(sample.cameraId, sample.cameraName);
  std::scoped_lock lock(privacyMutex_);
  privacyByCamera_[sample.cameraId] = {
      .state = sample.state,
      .obeyed = sample.obeyed || sample.state == CameraPrivacyState::On};
}

bool GuardService::cameraMasked(int64_t cameraId) const
{
  std::scoped_lock lock(privacyMutex_);
  const auto found = privacyByCamera_.find(cameraId);
  return found != privacyByCamera_.end() && found->second.obeyed;
}

GuardService::HouseholdPosture GuardService::householdPosture() const
{
  return {.protects = householdProtects_.load(std::memory_order_acquire),
          .armed = householdArmed_.load(std::memory_order_acquire)};
}

drogon::Task<void> GuardService::refreshHouseholdPosture(int64_t now)
{
  try {
    const int64_t environmentId = co_await environmentRepository_.defaultId();
    const auto environment = co_await environmentRepository_.find(environmentId);
    if (!environment)
      co_return;
    const GuardSchedule schedule =
        guard_schedule::fromEnvironment(*environment);
    const GuardPosture posture =
        postureAt({.schedule = schedule, .manual = environment->mode, .now = now});
    householdProtects_.store(protectionExpected(posture.mode),
                             std::memory_order_release);
    householdArmed_.store(posture.mode == GuardMode::Armed,
                          std::memory_order_release);
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard service: the household posture could not be resolved: "
             << error.what();
  }
  co_return;
}

std::vector<GuardService::MaskedCamera> GuardService::maskedCameras() const
{
  std::vector<MaskedCamera> masked;
  {
    std::scoped_lock lock(privacyMutex_);
    for (const auto& [cameraId, privacy] : privacyByCamera_) {
      if (privacy.obeyed)
        masked.push_back({.cameraId = cameraId, .name = {}});
    }
  }
  for (auto& camera : masked)
    camera.name = cameraName(camera.cameraId);
  return masked;
}

std::string GuardService::cameraHealthStatus(int64_t cameraId) const
{
  const auto config = currentConfig();
  std::scoped_lock lock(healthMutex_);
  const auto found = healthByCamera_.find(cameraId);
  if (found == healthByCamera_.end())
    return {};
  if (nowMillis() - found->second.lastSeenMs > config->healthStaleS * 1000)
    return {};
  return found->second.status;
}

drogon::Task<void> GuardService::closeTamperForPrivacy(int64_t cameraId,
                                                       int64_t now)
{
  const std::string onsetKey = "tamper_onset_" + std::to_string(cameraId);
  const std::string stored = co_await repository_.state(onsetKey, "");
  int64_t onset = 0;
  const auto [end, error] =
      std::from_chars(stored.data(), stored.data() + stored.size(), onset);
  if (error != std::errc{} || end != stored.data() + stored.size() || onset <= 0)
    co_return;
  co_await repository_.clearState(onsetKey);
  co_await repository_.clearState("tamper_last_seen_" +
                                  std::to_string(cameraId));
  co_await repository_.clearState("tamper_notified_onset_" +
                                  std::to_string(cameraId));
  co_await repository_.setState(
      {.key = "tamper_closed_" + std::to_string(cameraId),
       .value = "privacy",
       .updatedAt = now});
  LOG_INFO << "Guard tamper: camera " << cameraId
           << " is in privacy mode; its tamper episode is closed and it is "
              "not watched until the mask is off";
}

drogon::Task<void> GuardService::checkPrivacyCoverage(int64_t now)
{
  if (!evaluating())
    co_return;
  const auto masked = maskedCameras();
  if (masked.empty())
    co_return;
  struct MaskedEnvironment
  {
    GuardEnvironmentScope scope;
    std::vector<DigestLine> cameras;
  };
  std::map<int64_t, MaskedEnvironment> byEnvironment;
  for (const auto& camera : masked) {
    const GuardEnvironmentScope scope =
        co_await environmentScope(camera.cameraId);
    MaskedEnvironment& entry = byEnvironment[scope.environment.id];
    entry.scope = scope;
    entry.cameras.push_back({.cameraId = camera.cameraId,
                             .cameraName = camera.name,
                             .count = 0});
  }
  for (auto& [environmentId, entry] : byEnvironment) {
    std::vector<DigestLine>& cameras = entry.cameras;
    const GuardEnvironmentScope& scope = entry.scope;
    const GuardSchedule schedule =
        guard_schedule::fromEnvironment(scope.environment);
    const GuardPosture posture =
        postureAt({.schedule = schedule, .manual = scope.environment.mode, .now = now});
    if (!protectionExpected(posture.mode))
      continue;
    std::ranges::sort(cameras, {}, &DigestLine::cameraId);
    std::string fingerprint = guardModeToString(posture.mode);
    for (const auto& camera : cameras)
      fingerprint += ":" + std::to_string(camera.cameraId);
    const std::string key =
        "privacy_coverage_" + std::to_string(environmentId);
    if (co_await repository_.state(key, "") == fingerprint)
      continue;
    const GuardNotice notice{
        .kind = NoticeKind::PrivacyCoverage,
        .subject = NoticeSubject::Unobserved,
        .people = 0,
        .cameraId = 0,
        .cameraName = {},
        .environmentName = scope.several ? scope.environment.name : std::string{},
        .role = CameraRole::Other,
        .outdoor = false,
        .zoneName = {},
        .reasons = {},
        .dwellS = 0,
        .danger = GuardDanger::Low,
        .action = NoticeAction::Watching,
        .tamperStatus = {},
        .held = {},
        .routine = {},
        .masked = cameras,
        .notified = 0,
        .afterQuiet = false,
        .actorName = {},
        .visitor = {}};
    const Json::Value data = noticeData({.notice = notice,
                                         .environmentId = environmentId,
                                         .digestDay = {},
                                         .rule = "camera_privacy_coverage",
                                         .incidentId = 0,
                                         .encounterId = 0,
                                         .zoneKind = {},
                                         .identityState = {},
                                         .phase = "coverage"});
    const auto config = currentConfig();
    const GuardDanger authorized =
        guardDangerFromRank(std::clamp(config->notifyLevel, 1, 4));
    const EffectResult sent = co_await performEffect(
        {.kind = GuardActionKind::Notify,
         .danger = authorized,
         .greetingEnabled = false,
         .replyRequested = false,
         .cameraId = 0,
         .incidentId = 0,
         .encounterId = 0,
         .personId = 0,
         .now = now,
         .text = {},
         .lang = {},
         .seconds = 0,
         .correlationId = "privacy_coverage:" + std::to_string(environmentId) +
                          ":" + std::to_string(now),
         .sequence = 1,
         .notifyContent = {.notice = notice, .data = data, .excludeUserIds = {}}});
    if (sent.resumable)
      continue;
    co_await repository_.setState(
        {.key = key, .value = fingerprint, .updatedAt = now});
  }
  co_return;
}

drogon::Task<GuardService::SeenPerson> GuardService::seenNear(const SeenLookup& input)
{
  const auto config = currentConfig();
  std::vector<KnownSighting> window;
  {
    std::scoped_lock lock(sightingMutex_);
    const auto found = knownSightings_.find(input.cameraId);
    if (found != knownSightings_.end())
      for (const KnownSighting& sighting : found->second)
        if (sighting.atMs <= input.onsetMs + 1000 &&
            input.onsetMs - sighting.atMs <= config->tamper.seenWindowS * 1000)
          window.push_back(sighting);
  }
  std::map<int64_t, std::pair<double, double>> shares;
  for (const KnownSighting& sighting : window) {
    auto [entry, inserted] = shares.try_emplace(sighting.personId, sighting.share, sighting.share);
    if (!inserted)
      entry->second.second = sighting.share;
  }
  int64_t personId = 0;
  double best = 0.0;
  for (const auto& [candidate, share] : shares) {
    if (!guard_tamper::approachingLens(
            {.firstShare = share.first, .lastShare = share.second, .nearShare = config->tamper.nearShare}))
      continue;
    if (share.second > best) {
      best = share.second;
      personId = candidate;
    }
  }
  if (personId <= 0 || dependencies_.identity == nullptr)
    co_return SeenPerson{};
  const auto profile = co_await BlockingTask<std::optional<PersonProfile>>(
      [this, personId]() { return dependencies_.identity->getPerson(personId); });
  if (!profile)
    co_return SeenPerson{};
  SeenPerson seen{.found = true,
                  .personId = personId,
                  .userId = profile->userId.value_or(0),
                  .name = profile->name.empty() ? profile->alias : profile->name,
                  .role = UserRole::Unknown};
  if (seen.userId > 0)
    if (const auto users = co_await directoryUsers())
      for (const ResponseUser& user : *users)
        if (user.userId == seen.userId && user.active)
          seen.role = user.role;
  co_return seen;
}

drogon::Task<bool> GuardService::userAway(const PresenceLookup& input)
{
  if (input.userId <= 0)
    co_return false;
  try {
    const auto rows = co_await presenceRepository_.forEnvironment(
        {.environmentId = input.environmentId, .userIds = {input.userId}});
    co_return presence::stateOf(rows, input.userId) == PresenceState::Away;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard tamper: presence read failed: " << error.what();
  }
  co_return false;
}

drogon::Task<std::vector<ResponseUser>> GuardService::householdAtHome(int64_t environmentId)
{
  std::vector<ResponseUser> home;
  const auto users = co_await directoryUsers();
  if (!users)
    co_return home;
  std::vector<PresenceRow> rows;
  try {
    rows = co_await presenceRepository_.forEnvironment({.environmentId = environmentId, .userIds = {}});
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard tamper: presence read failed: " << error.what();
    co_return home;
  }
  for (const ResponseUser& user : *users)
    if (user.active && (user.role == UserRole::Owner || user.role == UserRole::Resident) &&
        presence::stateOf(rows, user.userId) == PresenceState::Home)
      home.push_back(user);
  co_return home;
}

drogon::Task<bool> GuardService::askHousehold(const TamperRaise& input)
{
  if (maintenance_ == nullptr)
    co_return false;
  const std::vector<ResponseUser> home = co_await householdAtHome(input.scope.environment.id);
  if (home.empty())
    co_return false;
  MaintenanceRepository asks;
  std::vector<int64_t> userIds;
  userIds.reserve(home.size());
  for (const ResponseUser& user : home)
    userIds.push_back(user.userId);
  const TamperAsk ask = co_await asks.createAsk({.cameraId = input.cameraId,
                                                 .environmentId = input.scope.environment.id,
                                                 .onsetAt = input.onset,
                                                 .askedAt = input.now,
                                                 .askedUsers = userIds});
  GuardNotice notice;
  notice.kind = NoticeKind::TamperAsk;
  notice.cameraId = input.cameraId;
  notice.cameraName = cameraName(input.cameraId);
  notice.environmentName = input.scope.several ? input.scope.environment.name : std::string{};
  notice.danger = GuardDanger::Medium;
  notice.tamperStatus = input.status;
  notice.onsetAt = input.onset;
  notice.askId = ask.id;
  const Json::Value data = noticeData({.notice = notice,
                                       .environmentId = input.scope.environment.id,
                                       .digestDay = {},
                                       .rule = "camera_tamper",
                                       .incidentId = 0,
                                       .encounterId = 0,
                                       .zoneKind = {},
                                       .identityState = {},
                                       .phase = "ask"});
  LOG_INFO << "Guard tamper: camera " << input.cameraId << " lost its sight at home; asking "
           << home.size() << " person(s) at home";
  static_cast<void>(co_await targeted_notice::send({.client = dependencies_.notifications,
                                                    .users = home,
                                                    .notice = notice,
                                                    .data = data,
                                                    .commandId = "tamper:ask:" + std::to_string(ask.id)}));
  co_return true;
}

drogon::Task<void> GuardService::settleAsks(int64_t now)
{
  const auto config = currentConfig();
  MaintenanceRepository asks;
  for (const TamperAsk& ask : co_await asks.pendingAsks())
    if (now - ask.askedAt >= config->tamper.askAnswerS)
      static_cast<void>(co_await asks.answerAsk(
          {.id = ask.id, .answer = TamperAskAnswer::Timeout, .answeredBy = 0, .at = now}));
}

drogon::Task<bool> GuardService::raiseTamper(const TamperRaise& input)
{
  const auto config = currentConfig();
  const std::string name = cameraName(input.cameraId);
  Json::Value tamperEvent(Json::objectValue);
  tamperEvent["status"] = input.lost.size() >= 2 ? std::string("group") : input.status;
  tamperEvent["mode"] = guardModeToString(input.mode);
  tamperEvent["onsetAt"] = static_cast<Json::Int64>(input.onset);
  tamperEvent["seenPersonName"] = input.seenName;
  tamperEvent["maintenance"] = input.probable ? "probable" : "";
  Json::Value lostIds(Json::arrayValue);
  for (const auto& line : input.lost)
    lostIds.append(static_cast<Json::Int64>(line.cameraId));
  tamperEvent["cameraIds"] = lostIds;
  const int64_t incidentId = co_await repository_.insertIncidentForEvent(
      {.cameraId = input.cameraId,
       .cameraName = name,
       .rule = "camera_tamper",
       .danger = guard_policy::dangerToString(input.danger),
       .severity = input.danger == GuardDanger::Critical ? "critical" : "high",
       .personId = 0,
       .identity = {},
       .eventId = input.eventId,
       .eventJson = json_util::toString(tamperEvent),
       .createdAt = input.now});
  const GuardCameraContext camera = co_await cameraContextRepository_.find(input.cameraId);
  const GuardNotice notice{.kind = NoticeKind::Tamper,
                           .subject = NoticeSubject::Stranger,
                           .people = 0,
                           .cameraId = input.cameraId,
                           .cameraName = name,
                           .environmentName = input.scope.several ? input.scope.environment.name : std::string{},
                           .role = camera.role,
                           .outdoor = camera.outdoor,
                           .zoneName = {},
                           .reasons = {},
                           .dwellS = input.now - input.onset,
                           .danger = input.danger,
                           .action = NoticeAction::Watching,
                           .tamperStatus = input.status,
                           .held = {},
                           .routine = {},
                           .masked = {},
                           .notified = 0,
                           .afterQuiet = false,
                           .actorName = {},
                           .visitor = {},
                           .lost = input.lost,
                           .seenName = input.seenName,
                           .seenGuest = input.seenGuest,
                           .probableMaintenance = input.probable,
                           .canClaim = input.canClaim,
                           .onsetAt = input.onset,
                           .askId = 0,
                           .maintenanceId = 0};
  const NotifyContent content{.notice = notice,
                              .data = noticeData({.notice = notice,
                                                  .environmentId = input.scope.environment.id,
                                                  .digestDay = {},
                                                  .rule = "camera_tamper",
                                                  .incidentId = incidentId,
                                                  .encounterId = 0,
                                                  .zoneKind = {},
                                                  .identityState = {},
                                                  .phase = "opened"}),
                              .excludeUserIds = {}};
  co_await journalDecision({.eventId = input.eventId,
                            .encounterId = 0,
                            .incidentId = incidentId,
                            .cameraId = input.cameraId,
                            .observationId = {},
                            .danger = input.danger,
                            .hardFloor = false,
                            .beliefScore = 0,
                            .beliefSignals = {"camera_health_degraded"},
                            .beliefThreshold = 0,
                            .legacyWouldNotify = true,
                            .beliefWouldNotify = false,
                            .decisionMode = config->decisionMode,
                            .suppression = DecisionSuppression::None,
                            .suppressedKinds = {},
                            .reasons = {},
                            .noveltyScore = 0.0,
                            .repeatVisits = 0,
                            .quietHold = false,
                            .budgetHold = false,
                            .assessMs = 0,
                            .environmentId = input.scope.environment.id,
                            .at = input.now});
  const GuardDanger authorized = guardDangerFromRank(std::clamp(config->notifyLevel, 1, 4));
  const GuardDanger effectDanger =
      guardDangerRank(input.danger) >= guardDangerRank(authorized) ? input.danger : authorized;
  const EffectResult sent = co_await performEffect({.kind = GuardActionKind::Notify,
                                                    .danger = effectDanger,
                                                    .greetingEnabled = false,
                                                    .replyRequested = false,
                                                    .cameraId = input.cameraId,
                                                    .incidentId = incidentId,
                                                    .encounterId = 0,
                                                    .personId = 0,
                                                    .now = input.now,
                                                    .text = {},
                                                    .lang = {},
                                                    .seconds = 0,
                                                    .correlationId = input.eventId,
                                                    .sequence = 1,
                                                    .notifyContent = content});
  co_return sent.accepted;
}

drogon::Task<void> GuardService::checkTamperSweep(int64_t now)
{
  if (!evaluating())
    co_return;
  const auto config = currentConfig();
  struct TamperReading
  {
    int64_t cameraId{0};
    std::string status;
    bool sudden{false};
    std::string link;
  };
  std::vector<TamperReading> fresh;
  {
    std::scoped_lock lock(healthMutex_);
    for (const auto& [cameraId, health] : healthByCamera_) {
      if (now - health.lastSeenMs / 1000 > config->healthStaleS)
        continue;
      fresh.push_back({.cameraId = cameraId, .status = health.status, .sudden = health.sudden, .link = health.link});
    }
  }
  const auto readStamp = [this](const std::string& key) -> drogon::Task<int64_t> {
    const std::string stored = co_await repository_.state(key, "");
    int64_t stamp = 0;
    const auto [end, error] = std::from_chars(stored.data(), stored.data() + stored.size(), stamp);
    if (error != std::errc{} || end != stored.data() + stored.size())
      co_return 0;
    co_return stamp;
  };
  for (const auto& masked : maskedCameras())
    co_await closeTamperForPrivacy(masked.cameraId, now);
  std::vector<MaintenanceWindow> windows;
  if (maintenance_ != nullptr) {
    co_await maintenance_->expireDue(now);
    windows = co_await maintenance_->activeWindows(now);
  }
  co_await settleAsks(now);

  struct Tampering
  {
    TamperReading reading;
    int64_t onset{0};
    GuardEnvironmentScope scope;
    GuardPosture posture;
    bool protect{false};
    bool maintained{false};
  };
  std::vector<Tampering> tampering;
  for (const auto& reading : fresh) {
    if (cameraMasked(reading.cameraId))
      continue;
    const std::string id = std::to_string(reading.cameraId);
    const std::string onsetKey = "tamper_onset_" + id;
    const std::string seenKey = "tamper_last_seen_" + id;
    const std::string notifiedKey = "tamper_notified_onset_" + id;
    const bool lens = reading.status == "blurred";
    if (!guard_tamper::indicating(reading.status, reading.sudden) && !lens) {
      if (co_await readStamp(onsetKey) > 0 && now - co_await readStamp(seenKey) >= config->tamperSustainedS) {
        co_await repository_.clearState(onsetKey);
        co_await repository_.clearState(seenKey);
        co_await repository_.clearState(notifiedKey);
      }
      continue;
    }
    int64_t onset = co_await readStamp(onsetKey);
    if (onset <= 0) {
      onset = now;
      co_await repository_.setState({.key = onsetKey, .value = std::to_string(now), .updatedAt = now});
    }
    co_await repository_.setState({.key = seenKey, .value = std::to_string(now), .updatedAt = now});
    const GuardEnvironmentScope scope = co_await environmentScope(reading.cameraId);
    const GuardSchedule schedule = guard_schedule::fromEnvironment(scope.environment);
    const GuardPosture posture = postureAt({.schedule = schedule, .manual = scope.environment.mode, .now = now});
    const bool maintained = std::ranges::any_of(windows, [&reading](const MaintenanceWindow& window) {
      return !window.duress && window.covers(reading.cameraId);
    });
    tampering.push_back({.reading = reading,
                         .onset = onset,
                         .scope = scope,
                         .posture = posture,
                         .protect = protectionExpected(posture.mode),
                         .maintained = maintained});
  }

  std::vector<TamperReadingRef> refs;
  for (const Tampering& entry : tampering)
    if (!entry.maintained && guard_tamper::indicating(entry.reading.status, entry.reading.sudden))
      refs.push_back({.cameraId = entry.reading.cameraId,
                      .environmentId = entry.scope.environment.id,
                      .onsetAt = entry.onset});
  std::vector<int64_t> grouped;
  for (const TamperGroup& group : guard_tamper::groupsOf(refs, config->tamper.groupWindowS)) {
    std::string fingerprint = std::to_string(group.firstOnset);
    for (const int64_t cameraId : group.cameraIds)
      fingerprint += ":" + std::to_string(cameraId);
    const std::string key = "tamper_group_" + std::to_string(group.environmentId);
    grouped.insert(grouped.end(), group.cameraIds.begin(), group.cameraIds.end());
    if (co_await repository_.state(key, "") == fingerprint)
      continue;
    const auto first = std::ranges::find(tampering, group.cameraIds.front(),
                                         [](const Tampering& entry) { return entry.reading.cameraId; });
    if (first == tampering.end())
      continue;
    const bool anyoneHome = co_await familyInside(group.environmentId);
    const GuardDanger danger = guard_tamper::groupDanger({.protect = first->protect, .anyoneHome = anyoneHome});
    std::vector<DigestLine> lost;
    for (const int64_t cameraId : group.cameraIds)
      lost.push_back({.cameraId = cameraId, .cameraName = cameraName(cameraId), .count = 1});
    LOG_WARN << "Guard tamper: " << lost.size() << " cameras of environment " << group.environmentId
             << " lost their sight within " << config->tamper.groupWindowS << " s";
    const bool accepted = co_await raiseTamper({.cameraId = group.cameraIds.front(),
                                                .status = first->reading.status,
                                                .onset = group.firstOnset,
                                                .now = now,
                                                .danger = danger,
                                                .scope = first->scope,
                                                .mode = first->posture.mode,
                                                .lost = lost,
                                                .seenName = {},
                                                .seenGuest = false,
                                                .probable = false,
                                                .canClaim = false,
                                                .eventId = "tamper:group:" + std::to_string(group.environmentId) +
                                                           ":" + fingerprint});
    if (!accepted)
      continue;
    co_await repository_.setState({.key = key, .value = fingerprint, .updatedAt = now});
    for (const int64_t cameraId : group.cameraIds) {
      const auto member = std::ranges::find(tampering, cameraId,
                                            [](const Tampering& entry) { return entry.reading.cameraId; });
      if (member != tampering.end())
        co_await repository_.setState({.key = "tamper_notified_onset_" + std::to_string(cameraId),
                                       .value = std::to_string(member->onset),
                                       .updatedAt = now});
    }
  }

  for (const Tampering& entry : tampering) {
    const TamperReading& reading = entry.reading;
    const std::string id = std::to_string(reading.cameraId);
    const std::string notifiedKey = "tamper_notified_onset_" + id;
    if (co_await repository_.state(notifiedKey, "") == std::to_string(entry.onset))
      continue;
    if (std::ranges::find(grouped, reading.cameraId) != grouped.end())
      continue;
    const TamperWindow window = guard_tamper::windowFor({.status = reading.status,
                                                         .sudden = reading.sudden,
                                                         .link = reading.link,
                                                         .protect = entry.protect,
                                                         .windows = config->tamper,
                                                         .homeS = config->tamperSustainedS,
                                                         .offlineHomeS = config->offlineSustainedS});
    if (window.action == TamperAction::Ignore)
      continue;
    if (entry.maintained) {
      LOG_INFO << "Guard tamper: camera " << reading.cameraId << " is " << reading.status
               << " inside a declared maintenance window; no alarm";
      co_await repository_.setState({.key = notifiedKey, .value = std::to_string(entry.onset), .updatedAt = now});
      continue;
    }
    if (window.action == TamperAction::LensNotice) {
      if (now - entry.onset < window.afterS)
        continue;
      GuardNotice notice;
      notice.kind = NoticeKind::LensNotice;
      notice.cameraId = reading.cameraId;
      notice.cameraName = cameraName(reading.cameraId);
      notice.environmentName = entry.scope.several ? entry.scope.environment.name : std::string{};
      notice.danger = GuardDanger::Low;
      notice.tamperStatus = reading.status;
      notice.onsetAt = entry.onset;
      const Json::Value data = noticeData({.notice = notice,
                                           .environmentId = entry.scope.environment.id,
                                           .digestDay = {},
                                           .rule = "camera_lens",
                                           .incidentId = 0,
                                           .encounterId = 0,
                                           .zoneKind = {},
                                           .identityState = {},
                                           .phase = "notice"});
      const EffectResult sent = co_await performEffect(
          {.kind = GuardActionKind::Notify,
           .danger = guardDangerFromRank(std::clamp(config->notifyLevel, 1, 4)),
           .greetingEnabled = false,
           .replyRequested = false,
           .cameraId = reading.cameraId,
           .incidentId = 0,
           .encounterId = 0,
           .personId = 0,
           .now = now,
           .text = {},
           .lang = {},
           .seconds = 0,
           .correlationId = "lens:" + id + ":" + std::to_string(entry.onset),
           .sequence = 1,
           .notifyContent = {.notice = notice, .data = data, .excludeUserIds = {}}});
      if (sent.accepted || !sent.resumable)
        co_await repository_.setState({.key = notifiedKey, .value = std::to_string(entry.onset), .updatedAt = now});
      continue;
    }

    const SeenPerson seen = co_await seenNear({.cameraId = reading.cameraId, .onsetMs = entry.onset * 1000});
    bool personAway = false;
    if (seen.found)
      personAway = co_await userAway({.environmentId = entry.scope.environment.id, .userId = seen.userId});
    bool onDuty = false;
    if (seen.found && seen.role == UserRole::Guard && seen.userId > 0) {
      const std::vector<int64_t> duty = co_await responseRepository_.dutyEnvironments(seen.userId);
      onDuty = std::ranges::find(duty, entry.scope.environment.id) != duty.end();
    }
    const SeenVerdict verdict = guard_tamper::seenVerdict({.found = seen.found,
                                                           .role = seen.role,
                                                           .onDuty = onDuty,
                                                           .personAway = personAway,
                                                           .protect = entry.protect});
    bool escalateNow = false;
    if (!entry.protect && verdict == SeenVerdict::None) {
      MaintenanceRepository asks;
      const auto ask = co_await asks.askForOnset({.cameraId = reading.cameraId, .onsetAt = entry.onset});
      if (!ask) {
        const bool someoneHome = co_await familyInside(entry.scope.environment.id);
        if (guard_tamper::shouldAsk({.protect = entry.protect, .seen = verdict, .anyoneHome = someoneHome})) {
          const bool asked = co_await askHousehold({.cameraId = reading.cameraId,
                                 .status = reading.status,
                                 .onset = entry.onset,
                                 .now = now,
                                 .danger = GuardDanger::Medium,
                                 .scope = entry.scope,
                                 .mode = entry.posture.mode,
                                 .lost = {},
                                 .seenName = {},
                                 .seenGuest = false,
                                 .probable = false,
                                 .canClaim = false,
                                 .eventId = {}});
          if (asked)
            continue;
        }
      }
      else if (ask->answer == TamperAskAnswer::Pending || ask->answer == TamperAskAnswer::Me) {
        continue;
      }
      else if (ask->answer == TamperAskAnswer::No) {
        escalateNow = true;
      }
    }
    if (!escalateNow && now - entry.onset < window.afterS)
      continue;
    GuardDanger danger = entry.protect ? GuardDanger::Critical : GuardDanger::High;
    bool probable = false;
    switch (verdict) {
      case SeenVerdict::HomeNotice:
        danger = GuardDanger::Low;
        probable = true;
        break;
      case SeenVerdict::Lowered:
        danger = GuardDanger::High;
        probable = true;
        break;
      case SeenVerdict::KeptCritical:
        danger = GuardDanger::Critical;
        break;
      case SeenVerdict::GuestNamed:
      case SeenVerdict::None:
        break;
    }
    const bool accepted = co_await raiseTamper(
        {.cameraId = reading.cameraId,
         .status = reading.status,
         .onset = entry.onset,
         .now = now,
         .danger = danger,
         .scope = entry.scope,
         .mode = entry.posture.mode,
         .lost = {},
         .seenName = verdict == SeenVerdict::None ? std::string{} : seen.name,
         .seenGuest = verdict == SeenVerdict::GuestNamed,
         .probable = probable,
         .canClaim = entry.protect,
         .eventId = "tamper:" + id + ":" + std::to_string(entry.onset)});
    if (!accepted)
      continue;
    co_await repository_.setState({.key = notifiedKey, .value = std::to_string(entry.onset), .updatedAt = now});
  }
  co_return;
}

BeliefConfig GuardService::beliefConfig(int64_t cameraId) const
{
  const auto config = currentConfig();
  const int64_t now = nowMillis();
  {
    std::scoped_lock lock(beliefMutex_);
    const auto found = beliefCache_.find(cameraId);
    if (found != beliefCache_.end() &&
        now - found->second.resolvedAt < config->beliefRefreshS * 1000)
      return found->second.config;
  }
  BeliefConfig resolved = GuardConfig::resolveBelief(cameraId);
  {
    std::scoped_lock lock(beliefMutex_);
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
         .reasons = reasonsJson(input.reasons),
         .noveltyScore = input.noveltyScore,
         .repeatVisits = input.repeatVisits,
         .quietHold = input.quietHold,
         .budgetHold = input.budgetHold,
         .assessMs = input.assessMs,
         .environmentId = input.environmentId,
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
  const auto config = currentConfig();
  HoldResult holds;
  try {
    const guard_schedule::QuietWindow window = guard_schedule::quietWindow(
        {.environment = input.environment, .config = *config});
    if (!window.enabled || !input.legacyWouldNotify)
      co_return holds;
    if (guard_policy::dangerRank(input.danger) >=
        guard_policy::dangerRank(GuardDanger::High))
      co_return holds;
    std::tm parts{};
    const std::time_t at = static_cast<std::time_t>(input.now);
    if (localtime_r(&at, &parts) == nullptr)
      co_return holds;
    if (guard_schedule::inQuietHours(window, parts.tm_hour))
      holds.quiet = true;
    const int64_t midnight =
        input.now - (parts.tm_hour * 3600 + parts.tm_min * 60 + parts.tm_sec);
    if (co_await repository_.firedSince(
            {.environmentId = input.environment.id, .since = midnight}) >=
        config->quietDailyBudget)
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
  if (drogon::app().isRunning() && subscribeRetryTimer_.has_value())
    drogon::app().getLoop()->invalidateTimer(*subscribeRetryTimer_);
  const uint64_t timer =
      drogon::app().getLoop()->runEvery(5.0, [this, lifecycle = lifecycle_]() {
        const LifecycleGuard guard(lifecycle);
        if (!guard.alive() || subscribed_.load(std::memory_order_acquire))
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
      });
  subscribeRetryTimer_ = timer;
  trackTimer(timer);
}

void GuardService::startSweeps()
{
  if (sweepsStarted_)
    return;
  sweepsStarted_ = true;
  scheduleEncounterSweep();
  scheduleTamperSweep();
  scheduleRetentionSweep();
}

int64_t GuardService::retryBackoffAt(int attempts, int64_t nowMs) const
{
  const auto config = currentConfig();
  const int boundedStep = std::clamp(attempts > 0 ? attempts - 1 : 0, 0, 16);
  int64_t delay = static_cast<int64_t>(config->retryBaseMs) << boundedStep;
  if (config->retryMaxMs > 0)
    delay = std::min<int64_t>(delay, config->retryMaxMs);
  return nowMs + std::max<int64_t>(delay, 1);
}

void GuardService::accept(Delivery delivery)
{
  const uint64_t keepalive = keepalive_->hold(std::move(delivery.inProgress));
  enqueue({.payload = std::move(delivery.payload),
           .ack = std::move(delivery.ack),
           .nak = std::move(delivery.nak),
           .term = std::move(delivery.term),
           .delivered = delivery.delivered,
           .leased = false,
           .keepalive = keepalive});
}

size_t GuardService::keepDeliveriesAlive() const
{
  return keepalive_->touch();
}

void GuardService::enqueue(QueueEntry entry)
{
  const int64_t cameraId =
      json_util::fromString(entry.payload).get("cameraId", 0).asInt64();
  {
    std::scoped_lock lock(queueMutex_);
    QueueLane& lane = lanes_[cameraId];
    lane.entries.push_back(std::move(entry));
    if (lane.processing)
      return;
    lane.processing = true;
  }
  drogon::async_run([this, cameraId]() -> drogon::Task<void> {
    co_await processQueue(cameraId);
  });
}

drogon::Task<void> GuardService::processQueue(int64_t cameraId)
{
  const LifecycleGuard aliveGuard(lifecycle_);
  while (true) {
    QueueEntry entry;
    {
      std::scoped_lock lock(queueMutex_);
      QueueLane& lane = lanes_[cameraId];
      if (lane.entries.empty()) {
        lanes_.erase(cameraId);
        co_return;
      }
      entry = std::move(lane.entries.front());
      lane.entries.pop_front();
    }
    const KeepaliveRelease release(*keepalive_, entry.keepalive);
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
  const auto config = currentConfig();
  if (!dependencies_.bus)
    return;
  Json::Value payload;
  payload["service"] = "argus-guard";
  payload["enabled"] = config->enabled;
  const HouseholdPosture posture = householdPosture();
  payload["protects"] = posture.protects;
  payload["armed"] = posture.armed;
  payload["at"] = static_cast<Json::Int64>(std::time(nullptr));
  dependencies_.bus->publish(nats_subject::kGuardHeartbeat,
                             json_util::toString(payload));
}

void GuardService::publishKnownSeen(const KnownSeenInput& input)
{
  if (!dependencies_.bus)
    return;
  Json::Value payload(Json::objectValue);
  payload["eventId"] = input.eventId;
  payload["personId"] = static_cast<Json::Int64>(input.personId);
  payload["cameraId"] = static_cast<Json::Int64>(input.cameraId);
  payload["cameraName"] = input.cameraName;
  payload["environmentId"] = static_cast<Json::Int64>(input.environmentId);
  payload["environmentName"] = input.environmentName;
  payload["at"] = static_cast<Json::Int64>(input.at);
  payload["passerby"] = input.passerby;
  dependencies_.bus->publish(nats_subject::kGuardKnownSeen,
                             json_util::toString(payload));
}

void GuardService::scheduleTamperSweep()
{
  const auto config = currentConfig();
  const double every = static_cast<double>(std::max<int64_t>(1, config->tamper.sweepS));
  trackTimer(drogon::app().getLoop()->runEvery(every, [this, lifecycle = lifecycle_]() {
    if (tamperSweepRunning_.exchange(true, std::memory_order_acq_rel))
      return;
    drogon::async_run([this, lifecycle]() -> drogon::Task<void> {
      const LifecycleGuard guard(lifecycle);
      const SweepFlag running(tamperSweepRunning_);
      if (!guard.alive())
        co_return;
      try {
        co_await checkTamperSweep(static_cast<int64_t>(std::time(nullptr)));
      }
      catch (const std::exception& error) {
        LOG_WARN << "Guard service: tamper sweep failed: " << error.what();
      }
      catch (...) {
        LOG_WARN << "Guard service: tamper sweep failed with unknown error";
      }
      co_return;
    });
  }));
}

void GuardService::scheduleEncounterSweep()
{
  trackTimer(drogon::app().getLoop()->runEvery(60.0, [this, lifecycle = lifecycle_]() {
    if (encounterSweepRunning_.exchange(true, std::memory_order_acq_rel))
      return;
    drogon::async_run([this, lifecycle]() -> drogon::Task<void> {
      const LifecycleGuard guard(lifecycle);
      const SweepFlag running(encounterSweepRunning_);
      if (!guard.alive())
        co_return;
      try {
        const auto config = currentConfig();
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const auto stale = co_await repository_.staleEncounters(
            now - config->encounterTimeoutS);
        if (!stale.empty()) {
          co_await repository_.closeStaleEncounters(
              {.olderThan = now - config->encounterTimeoutS,
               .closedAt = now,
               .decisionMode = config->decisionMode});
        }
        co_await checkPrivacyCoverage(now);
        co_await refreshHouseholdPosture(now);
        co_await maybeSendDigests(now);
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
      static_cast<void>(co_await repository_.enqueueEncounterClosed(input));
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

bool GuardService::evaluating() const
{
  return !dependencies_.active || dependencies_.active();
}

drogon::Task<bool>
GuardService::handleEvent(HandleEventInput input)
{
  const auto config = currentConfig();
  const LifecycleGuard aliveGuard(lifecycle_);
  if (!aliveGuard.alive())
    co_return true;
  if (!evaluating())
    co_return true;
  const GuardEventSignals signals =
      guard_policy::parseObjectEvent(input.event);
  if (signals.cameraId <= 0 || signals.rule.empty())
    co_return true;

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  recordSightings({.cameraId = signals.cameraId, .event = input.event, .atMs = nowMillis()});
  if (maintenance_ != nullptr && input.event["objects"].isArray()) {
    const bool stranger = std::ranges::any_of(input.event["objects"], [](const Json::Value& object) {
      return object.get("class", "").asString() == "person" &&
             object.get("identity", "").asString() != "known";
    });
    if (stranger) {
      const GuardEnvironmentScope place = co_await environmentScope(signals.cameraId);
      co_await maintenance_->endForUnknownPerson({.environmentId = place.environment.id, .at = now});
    }
  }
  const std::string eventId = input.event.get("eventId", "").asString();
  if (eventId.empty()) {
    co_await applyObservation({.event = input.event,
                               .signals = signals,
                               .state = {},
                               .redelivered = input.delivered > 1});
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
  const int parkThreshold = std::max(1, config->maxObservationAttempts - 1);
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
  const bool redelivered =
      input.delivered > 1 || state.kind != ObservationClaimKind::New;
  const ObservationResult outcome = co_await applyObservation(
      {.event = input.event,
       .signals = signals,
       .state = std::move(state),
       .redelivered = redelivered});
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
  json["areaInUse"] = checkpoint.areaInUse;
  json["passerby"] = checkpoint.passerby;
  json["expectedArea"] = checkpoint.expectedArea;
  json["cameraRole"] = checkpoint.cameraRole;
  json["outdoor"] = checkpoint.outdoor;
  json["familyInside"] = checkpoint.familyInside;
  if (checkpoint.visitor.present())
    json["visitor"] = visitorToJson(checkpoint.visitor);
  Json::Value reasons(Json::arrayValue);
  for (const auto& reason : checkpoint.reasons)
    reasons.append(reason);
  json["reasons"] = std::move(reasons);
  json["holdsComputed"] = checkpoint.holdsComputed;
  json["quietHold"] = checkpoint.quietHold;
  json["budgetHold"] = checkpoint.budgetHold;
  json["groupedInto"] = static_cast<Json::Int64>(checkpoint.groupedInto);
  json["visitCount"] = checkpoint.visitCount;
  json["expectedGuest"] = checkpoint.expectedGuest;
  json["guestId"] = static_cast<Json::Int64>(checkpoint.guestId);
  json["guestOneTime"] = checkpoint.guestOneTime;
  json["hardFloor"] = checkpoint.hardFloor;
  json["observedOnly"] = checkpoint.observedOnly;
  json["effectsDenied"] = checkpoint.effectsDenied;
  json["effectsStarted"] = checkpoint.effectsStarted;
  json["weapon"] = checkpoint.weapon;
  json["lateAssessed"] = checkpoint.lateAssessed;
  json["lateRaised"] = checkpoint.lateRaised;
  json["stale"] = checkpoint.stale;
  json["policyDanger"] = checkpoint.policyDanger;
  json["guestFloor"] = checkpoint.guestFloor;
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
  checkpoint.areaInUse = json.get("areaInUse", false).asBool();
  checkpoint.passerby = json.get("passerby", false).asBool();
  checkpoint.expectedArea = json.get("expectedArea", false).asBool();
  checkpoint.cameraRole = json.get("cameraRole", "").asString();
  checkpoint.outdoor = json.get("outdoor", false).asBool();
  checkpoint.familyInside = json.get("familyInside", false).asBool();
  checkpoint.visitor = visitorFromJson(json["visitor"]);
  for (const auto& reason : json["reasons"]) {
    if (reason.isString())
      checkpoint.reasons.push_back(reason.asString());
  }
  checkpoint.holdsComputed = json.get("holdsComputed", false).asBool();
  checkpoint.quietHold = json.get("quietHold", false).asBool();
  checkpoint.budgetHold = json.get("budgetHold", false).asBool();
  checkpoint.groupedInto = json.get("groupedInto", -1).asInt64();
  checkpoint.visitCount = json.get("visitCount", 0).asInt();
  checkpoint.expectedGuest = json.get("expectedGuest", false).asBool();
  checkpoint.guestId = json.get("guestId", 0).asInt64();
  checkpoint.guestOneTime = json.get("guestOneTime", false).asBool();
  checkpoint.hardFloor = json.get("hardFloor", false).asBool();
  checkpoint.observedOnly = json.get("observedOnly", false).asBool();
  checkpoint.effectsDenied = json.get("effectsDenied", false).asBool();
  checkpoint.effectsStarted = json.get("effectsStarted", false).asBool();
  checkpoint.weapon = json.get("weapon", false).asBool();
  checkpoint.lateAssessed = json.get("lateAssessed", false).asBool();
  checkpoint.lateRaised = json.get("lateRaised", false).asBool();
  checkpoint.stale = json.get("stale", false).asBool();
  checkpoint.policyDanger = json.get("policyDanger", "").asString();
  checkpoint.guestFloor = json.get("guestFloor", "").asString();
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
  const auto config = currentConfig();
  const Json::Value& event = input.event;
  const GuardEventSignals& signals = input.signals;
  ObservationClaim state = std::move(input.state);
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string eventId = event.get("eventId", "").asString();

  ObservationCheckpoint checkpoint =
      checkpointFromJson(json_util::fromString(state.checkpoint));

  const GuardEnvironmentScope scope =
      co_await environmentScope(signals.cameraId);
  const GuardEnvironment& environment = scope.environment;
  const std::string environmentName =
      scope.several ? environment.name : std::string{};
  const GuardSchedule schedule = guard_schedule::fromEnvironment(environment);
  const GuardPosture posture = postureAt(
      {.schedule = schedule, .manual = environment.mode, .now = now});
  const GuardMode mode = posture.mode;
  rememberCameraName(signals.cameraId, signals.cameraName);

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
    context.inAlertZone = alertZone(signals);
    context.atNight = signals.night || signals.rule == "person_night" ||
                      signals.rule == "vehicle_night";
    context.publicPresent = posture.publicPresent;
    context.staffOnly = posture.staffOnly;
    context.escalated =
        signals.escalated || signals.rule == "presence_escalating";
    context.strangerCount = signals.strangerCount;
    const GuardCameraContext camera =
        co_await cameraContextRepository_.find(signals.cameraId);
    const auto localAt = static_cast<std::time_t>(now);
    std::tm local{};
    localtime_r(&localAt, &local);
    const GuardArea area = guard_context::evaluate(
        {.camera = camera,
         .posture = posture,
         .local = local,
         .inAlertZone = context.inAlertZone});
    context.areaInUse = area.inUse;
    context.passerby = area.passerby;
    context.afterHours = posture.occupancy == "closed";
    checkpoint.areaInUse = area.inUse;
    checkpoint.passerby = area.passerby;
    checkpoint.expectedArea = posture.publicPresent || area.inUse;
    checkpoint.cameraRole = cameraRoleToString(camera.role);
    checkpoint.stale = input.redelivered && config->staleObservationS > 0 &&
                       signals.publishedAtMs > 0 &&
                       nowMillis() - signals.publishedAtMs > config->staleObservationS * 1000;
    checkpoint.outdoor = camera.outdoor;
    checkpoint.familyInside = camera.configured && !camera.outdoor &&
                              co_await familyInside(environment.id);
    checkpoint.visitor = {};
    if (signals.personId > 0 && dependencies_.identity) {
      const auto primary = co_await BlockingTask<std::optional<PersonProfile>>(
          [this, personId = signals.personId]() {
            return dependencies_.identity->getPerson(personId);
          });
      if (primary) {
        checkpoint.visitor = visitorOf(*primary, false);
        context.watchlist = primary->category == "watchlist";
      }
    }
    if (signals.hasUnknown && signals.knownPersonId > 0 &&
        dependencies_.identity) {
      const auto known = co_await BlockingTask<std::optional<PersonProfile>>(
          [this, personId = signals.knownPersonId]() {
            return dependencies_.identity->getPerson(personId);
          });
      const bool resident =
          known && known->userId.has_value() &&
          (known->role == "owner" || known->role == "resident");
      context.accompaniedByResident = resident;
      context.accompaniedByGuest = known.has_value() && !resident;
      if (known && !checkpoint.visitor.present())
        checkpoint.visitor = visitorOf(*known, true);
    }
    if (signals.personId > 0)
      context.visitCount =
          co_await repository_.repeatCount(signals.personId,
                                           now - config->repeatWindowS);
    if (config->expectedGuestsEnabled && signals.hasUnknown) {
      const auto guest = co_await repository_.activeGuest(
          {.at = now,
           .cameraId = signals.cameraId,
           .environmentId = environment.id,
           .personId = signals.personId});
      if (guest) {
        context.expectedGuest = true;
        if (guest->oneTime) {
          checkpoint.guestId = guest->id;
          checkpoint.guestOneTime = true;
        }
      }
    }
    const GuardDanger policyDanger = guard_policy::evaluate(context);
    checkpoint.reasons.clear();
    for (const GuardReason reason : guard_policy::explain(context))
      addReason(checkpoint.reasons, reason);
    if (signals.hasUnknown &&
        signals.identityState == IdentityState::Unobservable)
      addReason(checkpoint.reasons, GuardReason::FaceHidden);
    checkpoint.policyDanger = guard_policy::dangerToString(policyDanger);
    checkpoint.guestFloor =
        guard_policy::dangerToString(guard_policy::guestFloor(context));
    checkpoint.visitCount = context.visitCount;
    checkpoint.expectedGuest = context.expectedGuest;
    checkpoint.hardFloor = guard_policy::dangerRank(policyDanger) >=
                           guard_policy::dangerRank(GuardDanger::High);
    hardFloor = checkpoint.hardFloor;
    danger = policyDanger;
    if (signals.personId > 0 && dependencies_.identity &&
        (context.accompaniedByResident || context.accompaniedByGuest)) {
      const std::string source =
          context.accompaniedByResident ? "resident" : "guest";
      co_await BlockingTask<bool>([this, personId = signals.personId,
                                   source]() {
        return dependencies_.identity->tagPerson(
            {.personId = personId,
             .tags = {"companion"},
             .source = "guard",
             .observation = "accompanied by " + source});
      });
    }
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
                          .identity = signals.hasKnown && !signals.hasUnknown
                                       ? "known"
                                       : "unknown",
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
    if (phase.guestTaken) {
      LOG_INFO << "Guard service: one-time pass already used; observation "
               << eventId << " is evaluated again without it";
      co_return ObservationResult{.completed = false, .retryAt = nowMillis()};
    }
    if (!phase.committed) {
      throw std::runtime_error("incident phase commit failed");
    }
    state.incidentId = phase.incidentId;
    state.stage = kStageIncident;
    failAt("after_incident");
  }

  if (state.stage < kStageEncounter) {
    GuardEncounterPhaseInput phase;
    phase.advance = {.eventId = eventId,
                     .stage = kStageEncounter,
                     .incidentId = state.incidentId,
                     .encounterId = 0,
                     .danger = checkpoint.danger,
                     .checkpoint =
                         json_util::toString(checkpointToJson(checkpoint)),
                     .at = now};
    phase.match = GuardEncounterMatchSpec{
        .personId = signals.personId,
        .signature = signals.signature,
        .cameraId = signals.cameraId,
        .bestScore = signals.viewScore,
        .now = now,
        .windowS = config->crossCameraWindowS,
        .continuityWindowS = config->continuityWindowS,
        .minSimilarity = config->signatureMinSimilarity,
        .createIfUnmatched = signals.hasUnknown};
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
    if (encounterPhase.encounterId > 0 &&
        dependencies_.episodeChanges != nullptr) {
      if (encounterPhase.created)
        dependencies_.episodeChanges->published(encounterPhase.encounterId);
      else
        co_await dependencies_.episodeChanges->touch(encounterPhase.encounterId);
    }
    for (const auto& encounter : encounterPhase.closed)
      publishEncounterClosed(encounter, now);
    if (signals.hasKnown && !signals.hasUnknown && signals.personId > 0)
      publishKnownSeen({.eventId = eventId,
                        .personId = signals.personId,
                        .cameraId = signals.cameraId,
                        .cameraName = signals.cameraName,
                        .environmentId = environment.id,
                        .environmentName = environmentName,
                        .at = now,
                        .passerby = checkpoint.passerby});
    state.stage = kStageEncounter;
    failAt("after_encounter");
  }

  if (state.stage < kStageDialogue) {
    const bool knownVisit = signals.hasKnown && !signals.hasUnknown &&
                            signals.personId > 0 && config->greetKnown;
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
    if (config->greetEnabled && mode == GuardMode::Home &&
        !posture.publicPresent && !posture.staffOnly &&
        !checkpoint.expectedArea && !checkpoint.passerby &&
        (knownVisit || unknownVisit || continuesDialogue) && !hardFloor &&
        !checkpoint.stale) {
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
    if (dependencies_.assessment && signals.hasUnknown && !hardFloor)
      danger = co_await assessInto({.signals = signals,
                                    .checkpoint = checkpoint,
                                    .danger = danger,
                                    .hardFloor = hardFloor,
                                    .assessMs = assessMs,
                                    .profile = environmentKindToString(
                                        environment.kind)});
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
    const bool vetoed = checkpoint.assessment.performed &&
                        checkpoint.assessment.veto;
    if (config->stagingEnabled && signals.hasUnknown && !hardFloor &&
        guard_policy::dangerRank(danger) <
            guard_policy::dangerRank(GuardDanger::High)) {
      observedOnly = checkpoint.encounterChecks < config->loiterChecks;
      if (!observedOnly && !vetoed && !checkpoint.expectedArea) {
        danger = guard_policy::dangerRank(danger) <
                         guard_policy::dangerRank(GuardDanger::Medium)
                     ? GuardDanger::Medium
                     : GuardDanger::High;
        addReason(checkpoint.reasons, GuardReason::Lingering);
      }
    }
    if (observedOnly)
      addReason(checkpoint.reasons, GuardReason::Brief);
    checkpoint.observedOnly = observedOnly;
    checkpoint.danger = guard_policy::dangerToString(danger);

    co_await repository_.updateIncidentDanger(state.incidentId,
                                               checkpoint.danger);

    if (checkpoint.observedOnly) {
      const int stagedRank = guard_policy::dangerRank(danger);
      const bool stagedLegacyWould = stagedRank >= config->notifyLevel;
      std::vector<std::string> stagedKinds;
      if (stagedRank >= config->notifyLevel)
        stagedKinds.push_back("notify");
      if (stagedRank >= config->announceLevel)
        stagedKinds.push_back("announce");
      if (stagedRank >= config->alarmLevel) {
        stagedKinds.push_back("alarm");
        stagedKinds.push_back("siren_arm");
      }
      const CollectionResult stagedCollected = co_await collectSignals(
          {.cameraId = signals.cameraId,
           .signature = signals.signature,
           .hasUnknown = signals.hasUnknown,
           .now = now});
      const HoldResult stagedHolds = co_await computeHolds(
          {.environment = environment,
           .danger = danger,
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
           .decisionMode = config->decisionMode,
           .suppression = DecisionSuppression::Staging,
           .suppressedKinds = stagedKinds,
           .reasons = checkpoint.reasons,
           .noveltyScore = stagedCollected.noveltyScore,
           .repeatVisits = stagedCollected.repeatVisits,
           .quietHold = stagedHolds.quiet,
           .budgetHold = stagedHolds.budget,
           .assessMs = 0,
           .environmentId = environment.id,
           .at = now});
      if (state.encounterId > 0)
        co_await episodeRepository_.recordInsight(
            {.encounterId = state.encounterId,
             .subject = noticeSubjectToString(subjectFor(signals)),
             .people = std::max(1, signals.unknownCount),
             .reasons = reasonsJson(checkpoint.reasons),
             .rank = guard_policy::dangerRank(danger),
             .environmentId = environment.id});
    }

    co_await recordAssessment({.signals = signals,
                               .checkpoint = checkpoint,
                               .incidentId = state.incidentId,
                               .encounterId = state.encounterId,
                               .eventId = eventId,
                               .now = now});

    if (!hardFloor && checkpoint.dialogue.greeted &&
        !checkpoint.dialogue.replied && config->greetReplyEnabled &&
        guard_policy::dangerRank(danger) <
            guard_policy::dangerRank(GuardDanger::High) &&
        (!checkpoint.dialogue.heardText.empty() ||
         checkpoint.dialogue.repairPending)) {
      std::vector<std::string> variants = config->greetReplyTexts;
      if (variants.empty() && !config->greetReplyText.empty())
        variants.push_back(config->greetReplyText);
      std::string grounded;
      if (checkpoint.assessment.performed && checkpoint.assessment.valid &&
          !checkpoint.assessment.announceText.empty())
        grounded = checkpoint.assessment.announceText;
      else if (checkpoint.dialogue.repairPending &&
               !config->greetRepairText.empty())
        grounded = config->greetRepairText;
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
                                    .lang = config->greetReplyLang,
                                    .correlationId = eventId,
                                    .sequence = 100,
                                    .notifyContent = {}});
        if (reply.resumable) {
          co_return ObservationResult{.completed = false,
                                      .retryAt = reply.retryAt};
        }
        checkpoint.dialogue.replied = reply.accepted;
        checkpoint.dialogue.replyText =
            reply.accepted ? grounded : std::string{};
        std::string nextGoal(kGoalDone);
        if (reply.accepted && checkpoint.dialogue.listenAfterReply &&
            config->greetListenSeconds > 0 && state.encounterId > 0) {
          const int seconds = std::clamp(config->greetListenSeconds, 1, 10);
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
                                      .lang = config->greetLang,
                                      .seconds = seconds,
                                      .correlationId = eventId,
                                      .sequence = 101,
                                      .notifyContent = {}});
          failAt("after_offer_listen");
          if (heard.resumable) {
            co_return ObservationResult{.completed = false,
                                        .retryAt = heard.retryAt};
          }
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
        {config->notifyLevel, config->announceLevel, config->alarmLevel});
    const bool effectsRequested = rank >= lowestLevel;
    if (effectsRequested) {
      bool escalating = false;
      if (state.encounterId > 0) {
        const auto encounter =
            co_await repository_.findEncounter(state.encounterId);
        escalating = encounter.has_value() && encounter->notifyCount > 0 &&
                     rank > encounter->notifyHighestRank;
      }
      const bool allowed = state.encounterId > 0
                               ? co_await repository_.canActEncounter(
                                     {.id = state.encounterId,
                                      .now = now,
                                      .cooldownS = escalating
                                                       ? 0
                                                       : config->actionCooldownS,
                                      .maxPerHour = config->maxActionsPerHour})
                               : co_await repository_.canAct(
                                     {.cameraId = signals.cameraId,
                                      .personId = signals.personId,
                                      .cooldownS = config->actionCooldownS,
                                      .now = now,
                                      .maxPerHour = config->maxActionsPerHour});
      if (!allowed) {
        checkpoint.effectsDenied = true;
        co_await repository_.insertAction({.incidentId = state.incidentId,
                                           .encounterId = state.encounterId,
                                           .cameraId = signals.cameraId,
                                           .personId = signals.personId,
                                           .commandId = {},
                                           .kind = guardActionKindToString(
                                               GuardActionKind::Notify),
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
  GuardDeterrence deterrent = guard_policy::deterrence(
      {.mode = mode,
       .danger = danger,
       .publicPresent = posture.publicPresent,
       .staffOnly = posture.staffOnly,
       .weapon = checkpoint.weapon,
       .inAlertZone = alertZone(signals),
       .encounterChecks = checkpoint.encounterChecks,
       .quietArea = checkpoint.expectedArea || checkpoint.passerby,
       .familyInside = checkpoint.familyInside});
  if (checkpoint.stale)
    deterrent = {};
  bool notifySuppressed = false;
  bool announceSuppressed = false;
  bool alarmSuppressed = false;
  if (state.stage < kStageEffects || checkpoint.effectsDenied) {
    const int rank = guard_policy::dangerRank(danger);
    const bool legacyWould = rank >= config->notifyLevel;
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
    const bool enforce =
        decisionModeFromString(config->decisionMode) == DecisionMode::Enforce;
    bool threadRepeat = false;
    bool threadOpen = false;
    if (legacyWould && !checkpoint.effectsDenied && state.encounterId > 0) {
      const auto encounter =
          co_await repository_.findEncounter(state.encounterId);
      threadOpen = encounter.has_value() && encounter->notifyCount > 0;
      threadRepeat = threadOpen && rank <= encounter->notifyHighestRank;
    }
    const bool notifyRequested = rank >= config->notifyLevel;
    const bool deferrable =
        rank < guard_policy::dangerRank(GuardDanger::High);
    bool decisionsComputed = false;
    if (!checkpoint.holdsComputed) {
      const HoldResult holds = co_await computeHolds(
          {.environment = environment,
           .danger = danger,
           .legacyWouldNotify = legacyWould,
           .now = now});
      checkpoint.holdsComputed = true;
      checkpoint.quietHold = holds.quiet;
      checkpoint.budgetHold = holds.budget;
      decisionsComputed = true;
    }
    if (checkpoint.groupedInto < 0) {
      checkpoint.groupedInto = 0;
      decisionsComputed = true;
      if (notifyRequested && deferrable && !threadOpen &&
          !checkpoint.effectsDenied && state.encounterId > 0 &&
          config->regroupWindowS > 0) {
        const auto recent = co_await episodeRepository_.recentCameraNotification(
            {.cameraId = signals.cameraId,
             .excludeEncounterId = state.encounterId,
             .since = now - config->regroupWindowS});
        if (recent && recent->rank >= rank)
          checkpoint.groupedInto = recent->encounterId;
      }
    }
    if (decisionsComputed)
      co_await advanceObservation({.eventId = eventId,
                                   .stage = state.stage,
                                   .incidentId = state.incidentId,
                                   .encounterId = state.encounterId,
                                   .checkpoint = checkpoint,
                                   .at = now});
    const bool grouped = notifyRequested && !threadRepeat &&
                         checkpoint.groupedInto > 0;
    const bool held = notifyRequested && !threadRepeat && !grouped &&
                      deferrable &&
                      (checkpoint.quietHold || checkpoint.budgetHold);
    const bool announceRequested =
        rank >= config->announceLevel && deterrent.voice;
    const bool alarmRequested = rank >= config->alarmLevel && deterrent.alarm;
    const bool beliefBlocks = enforce && !beliefWould;
    const auto scopeCovers = [scope = config->beliefGateScope,
                              hardFloor = checkpoint.hardFloor](
                                 GuardActionKind kind) {
      return guard_belief::beliefSuppressesKind(
          {.scope = scope,
           .kind = kind,
           .hardFloor = hardFloor});
    };
    notifySuppressed =
        notifyRequested &&
        (threadRepeat || grouped || held ||
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
    else if (grouped)
      suppression = DecisionSuppression::Grouped;
    else if (held)
      suppression = DecisionSuppression::Held;
    else if (notifySuppressed)
      suppression = DecisionSuppression::BeliefGate;
    else
      suppression = DecisionSuppression::None;
    suppressedKinds.clear();
    if ((threadRepeat || grouped || held) && notifyRequested)
      suppressedKinds.push_back("notify");
    if (beliefBlocks) {
      if (notifyRequested && scopeCovers(GuardActionKind::Notify) &&
          !threadRepeat && !grouped && !held)
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
           .decisionMode = config->decisionMode,
           .suppression = suppression,
           .suppressedKinds = suppressedKinds,
           .reasons = checkpoint.reasons,
           .noveltyScore = collected.noveltyScore,
           .repeatVisits = collected.repeatVisits,
           .quietHold = checkpoint.quietHold,
           .budgetHold = checkpoint.budgetHold,
           .assessMs = assessMs,
           .environmentId = environment.id,
           .at = now});
      if (state.encounterId > 0) {
        co_await episodeRepository_.recordInsight(
            {.encounterId = state.encounterId,
             .subject = noticeSubjectToString(subjectFor(signals)),
             .people = std::max(1, signals.unknownCount),
             .reasons = reasonsJson(checkpoint.reasons),
             .rank = rank,
             .environmentId = environment.id});
        if (grouped)
          co_await episodeRepository_.linkGroup(state.encounterId,
                                                checkpoint.groupedInto);
      }
    }
    if (journalFresh && state.stage < kStageEffects) {
      for (const auto& kind : suppressedKinds) {
        const bool notifyRow = kind == "notify";
        std::string status = "belief_suppressed";
        std::string detail = "belief_below_threshold";
        if (notifyRow && suppression == DecisionSuppression::ThreadSuppressed) {
          status = "thread_suppressed";
          detail = "same_tier_repeat";
        }
        else if (notifyRow && suppression == DecisionSuppression::Grouped) {
          status = "grouped";
          detail = "episode:" + std::to_string(checkpoint.groupedInto);
        }
        else if (notifyRow && suppression == DecisionSuppression::Held) {
          status = "held";
          detail = checkpoint.quietHold ? "quiet_hours" : "daily_budget";
        }
        co_await repository_.insertAction(
            {.incidentId = state.incidentId,
             .encounterId = state.encounterId,
             .cameraId = signals.cameraId,
             .personId = signals.personId,
             .commandId = {},
             .kind = kind,
             .status = status,
             .detail = detail,
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
    bool anyAccepted = false;
    int64_t retryAt = 0;
    const auto note = [&resumable, &retryAt,
                       &anyAccepted](const EffectResult& effect) {
      anyAccepted = anyAccepted || effect.accepted || effect.pending;
      if (!effect.resumable)
        return;
      resumable = true;
      retryAt =
          retryAt == 0 ? effect.retryAt : std::min(retryAt, effect.retryAt);
    };
    std::optional<ExecutionLease> threadLease;
    bool threadTaken = false;
    bool threadOpen = false;
    const bool notifyWanted = rank >= config->notifyLevel && !notifySuppressed;
    if (notifyWanted && state.encounterId > 0) {
      threadLease.emplace(ExecutionLeaseInput{
          .mutex = lifecycleMutex_,
          .running = executing_,
          .id = "notify-thread:" + std::to_string(state.encounterId)});
      if (!threadLease->owned()) {
        threadTaken = true;
        EffectResult busy;
        busy.resumable = true;
        busy.retryAt = nowMillis() + kThreadBusyRetryMs;
        note(busy);
      }
      else {
        const auto encounter =
            co_await repository_.findEncounter(state.encounterId);
        threadOpen = encounter.has_value() && encounter->notifyCount > 0;
        const std::string ownCommand =
            makeCommandId({.correlationId = eventId,
                           .kind = GuardActionKind::Notify,
                           .sequence = 1,
                           .cameraId = signals.cameraId,
                           .now = now});
        if (threadOpen && rank <= encounter->notifyHighestRank &&
            encounter->notifyCommandId != ownCommand) {
          threadTaken = true;
          co_await repository_.insertAction(
              {.incidentId = state.incidentId,
               .encounterId = state.encounterId,
               .cameraId = signals.cameraId,
               .personId = signals.personId,
               .commandId = {},
               .kind = guardActionKindToString(GuardActionKind::Notify),
               .status = "thread_suppressed",
               .detail = "same_tier_repeat",
               .createdAt = now});
        }
      }
    }
    if (notifyWanted && !threadTaken) {
      NoticeAction action = NoticeAction::Watching;
      if (rank >= config->alarmLevel && deterrent.alarm && !alarmSuppressed)
        action = NoticeAction::Alarm;
      else if (rank >= config->announceLevel && deterrent.voice &&
               !announceSuppressed)
        action = NoticeAction::Speaker;
      else if (checkpoint.dialogue.greeted)
        action = checkpoint.dialogue.heardText.empty()
                     ? NoticeAction::GreetedNoReply
                     : NoticeAction::Greeted;
      else if (checkpoint.weapon && !deterrent.voice)
        action = NoticeAction::SilentWeapon;
      const GuardNotice notice{
          .kind = threadOpen ? NoticeKind::Escalation : NoticeKind::Episode,
          .subject = subjectFor(signals),
          .people = std::max(1, signals.unknownCount),
          .cameraId = signals.cameraId,
          .cameraName = signals.cameraName,
          .environmentName = environmentName,
          .role = cameraRoleFromString(checkpoint.cameraRole)
                      .value_or(CameraRole::Other),
          .outdoor = checkpoint.outdoor,
          .zoneName = signals.zoneName,
          .reasons = reasonsFrom(checkpoint.reasons),
          .dwellS = std::max<int64_t>(0, signals.dwellMs) / 1000,
          .danger = danger,
          .action = action,
          .tamperStatus = {},
          .held = {},
          .routine = {},
          .notified = 0,
          .afterQuiet = false, .actorName = {}, .visitor = checkpoint.visitor};
      const NotifyContent content{
          .notice = notice,
          .data = noticeData(
              {.notice = notice,
               .environmentId = environment.id,
               .digestDay = {},
               .rule = signals.rule,
               .incidentId = state.incidentId,
               .encounterId = state.encounterId,
               .zoneKind = signals.zoneKind,
               .identityState = identityStateToString(signals.identityState),
               .phase = threadOpen ? "escalated" : "opened"}),
          .excludeUserIds = {}};
      note(co_await performEffect(
          {.kind = GuardActionKind::Notify,
           .danger = danger,
           .cameraId = signals.cameraId,
           .incidentId = state.incidentId,
           .encounterId = state.encounterId,
           .personId = signals.personId,
           .now = now,
           .correlationId = eventId,
           .sequence = 1,
           .notifyContent = content}));
    }
    threadLease.reset();

    if (rank >= config->alarmLevel && deterrent.alarm && !alarmSuppressed) {
      note(co_await performEffect({.kind = GuardActionKind::Alarm,
                                   .danger = danger,
                                   .cameraId = signals.cameraId,
                                   .incidentId = state.incidentId,
                                   .encounterId = state.encounterId,
                                   .personId = signals.personId,
                                   .now = now,
                                   .seconds = config->alarmSeconds,
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
    if (rank >= config->announceLevel && deterrent.voice &&
        !announceSuppressed) {
      const std::string text = sanitizeSpoken(
          config->announceText.empty() ? std::string{"Atencion: zona vigilada."}
                                       : config->announceText);
      if (text.empty()) {
        co_await repository_.insertAction({.incidentId = state.incidentId,
                                           .encounterId = state.encounterId,
                                           .cameraId = signals.cameraId,
                                           .personId = signals.personId,
                                           .commandId = {},
                                           .kind = guardActionKindToString(
                                               GuardActionKind::Announce),
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
                                     .lang = config->announceLang,
                                     .correlationId = eventId,
                                     .sequence = 2,
                                   .notifyContent = {}}));
    }

    if (anyAccepted && state.encounterId > 0)
      co_await repository_.markEncounterAction(state.encounterId, now);
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

  if (state.stage < kStageEvidence && hardFloor && signals.hasUnknown &&
      dependencies_.assessment) {
    if (!checkpoint.lateAssessed) {
      const GuardDanger before = danger;
      danger = co_await assessInto({.signals = signals,
                                    .checkpoint = checkpoint,
                                    .danger = danger,
                                    .hardFloor = true,
                                    .assessMs = assessMs,
                                    .profile = environmentKindToString(
                                        environment.kind)});
      co_await recordAssessment({.signals = signals,
                                 .checkpoint = checkpoint,
                                 .incidentId = state.incidentId,
                                 .encounterId = state.encounterId,
                                 .eventId = eventId,
                                 .now = now});
      checkpoint.lateAssessed = true;
      checkpoint.lateRaised =
          guard_policy::dangerRank(danger) > guard_policy::dangerRank(before);
      checkpoint.danger = guard_policy::dangerToString(danger);
      if (checkpoint.lateRaised)
        co_await repository_.updateIncidentDanger(state.incidentId,
                                                   checkpoint.danger);
      co_await advanceObservation({.eventId = eventId,
                                   .stage = state.stage,
                                   .incidentId = state.incidentId,
                                   .encounterId = state.encounterId,
                                   .checkpoint = checkpoint,
                                   .at = now});
    }
    danger = guard_policy::dangerFromString(checkpoint.danger);
    if (checkpoint.lateRaised) {
      const EscalationResult escalation = co_await escalate(
          {.signals = signals,
           .checkpoint = checkpoint,
           .posture = posture,
           .scope = scope,
           .danger = danger,
           .incidentId = state.incidentId,
           .encounterId = state.encounterId,
           .eventId = eventId,
           .now = now});
      if (escalation.resumable)
        co_return ObservationResult{.completed = false,
                                    .retryAt = escalation.retryAt};
    }
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

drogon::Task<GuardDanger>
GuardService::assessInto(const AssessIntoInput& input)
{
  const auto config = currentConfig();
  const GuardEventSignals& signals = input.signals;
  ObservationCheckpoint& checkpoint = input.checkpoint;
  GuardDanger danger = input.danger;
  const bool hardFloor = input.hardFloor;
  int& assessMs = input.assessMs;
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
       .profile = input.profile,
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
  if (!checkpoint.dialogue.heardText.empty())
    checkpoint.assessment.veto = false;
  const GuardDanger guestFloor =
      guard_policy::dangerFromString(checkpoint.guestFloor);
  const bool guestSoftened = guestFloor != GuardDanger::None;
  const bool soft = guard_policy::dangerRank(danger) <
                    guard_policy::dangerRank(GuardDanger::High);
  if (checkpoint.assessment.performed && checkpoint.assessment.veto &&
      config->vetoScope == "soft_only" && soft && !guestSoftened)
    danger = GuardDanger::Low;
  if (checkpoint.assessment.performed) {
    const GuardRiskResult risk =
        guard_risk::mergeEvidence({.floor = danger,
                                   .threat = checkpoint.assessment.threat,
                                   .tags = checkpoint.assessment.tags,
                                   .caption = checkpoint.assessment.caption,
                                   .hardFloor = hardFloor});
    danger = risk.danger;
    checkpoint.weapon = risk.weapon;
    checkpoint.assessment.tags = risk.appliedTags;
    if (guestSoftened &&
        guard_risk::revokesGuestPass({.threat = checkpoint.assessment.threat,
                                      .tags = risk.appliedTags}))
      danger = std::max(danger, guestFloor);
    if (risk.weapon) {
      std::vector<std::string> ordered{guardReasonToString(GuardReason::Weapon)};
      for (const auto& reason : checkpoint.reasons) {
        if (reason != ordered.front())
          ordered.push_back(reason);
      }
      checkpoint.reasons = std::move(ordered);
    }
  }
  co_return danger;
}

drogon::Task<void>
GuardService::recordAssessment(const RecordAssessmentInput& input)
{
  const GuardEventSignals& signals = input.signals;
  const ObservationCheckpoint& checkpoint = input.checkpoint;
  if (checkpoint.assessment.performed) {
    co_await repository_.insertAssessment(
        {.incidentId = input.incidentId,
         .cameraId = signals.cameraId,
         .eventId = input.eventId,
         .mode = checkpoint.assessment.mode,
         .caption = checkpoint.assessment.caption,
         .threat = checkpoint.assessment.threat,
         .veto = checkpoint.assessment.veto,
         .tags = tagsToJson(checkpoint.assessment.tags),
         .summary = checkpoint.assessment.summary,
         .createdAt = input.now});
    int logIndex = 0;
    for (const auto& log : checkpoint.assessment.toolLogs) {
      co_await repository_.insertAction(
          {.incidentId = input.incidentId,
           .encounterId = input.encounterId,
           .cameraId = signals.cameraId,
           .personId = signals.personId,
           .commandId = input.eventId + ":agent:" + log.kind + ":" +
                        std::to_string(logIndex++),
           .kind = "agent_" + log.kind,
           .status = log.status,
           .detail = {},
           .createdAt = input.now});
    }
  }
}

drogon::Task<GuardService::EscalationResult>
GuardService::escalate(const EscalateInput& input)
{
  const auto config = currentConfig();
  const GuardEventSignals& signals = input.signals;
  EscalationResult result;
  const auto note = [&result](const EffectResult& effect) {
    if (!effect.resumable)
      return;
    result.resumable = true;
    result.retryAt = result.retryAt == 0
                         ? effect.retryAt
                         : std::min(result.retryAt, effect.retryAt);
  };
  const std::string correlation = input.eventId + ":escalation";
  const int rank = guard_policy::dangerRank(input.danger);
  GuardDeterrence deterrent = guard_policy::deterrence(
      {.mode = input.posture.mode,
       .danger = input.danger,
       .publicPresent = input.posture.publicPresent,
       .staffOnly = input.posture.staffOnly,
       .weapon = input.checkpoint.weapon,
       .inAlertZone = alertZone(signals),
       .encounterChecks = input.checkpoint.encounterChecks,
       .quietArea = input.checkpoint.expectedArea || input.checkpoint.passerby,
       .familyInside = input.checkpoint.familyInside});
  if (input.checkpoint.stale)
    deterrent = {};

  bool threadRepeat = false;
  if (input.encounterId > 0) {
    const auto encounter = co_await repository_.findEncounter(input.encounterId);
    threadRepeat = encounter.has_value() && encounter->notifyCount > 0 &&
                   rank <= encounter->notifyHighestRank;
  }

  if (rank >= config->notifyLevel && !threadRepeat) {
    NoticeAction action = NoticeAction::Watching;
    if (rank >= config->alarmLevel && deterrent.alarm)
      action = NoticeAction::Alarm;
    else if (rank >= config->announceLevel && deterrent.voice)
      action = NoticeAction::Speaker;
    else if (input.checkpoint.weapon)
      action = NoticeAction::SilentWeapon;
    const GuardNotice notice{
        .kind = NoticeKind::Escalation,
        .subject = subjectFor(signals),
        .people = std::max(1, signals.unknownCount),
        .cameraId = signals.cameraId,
        .cameraName = signals.cameraName,
        .environmentName =
            input.scope.several ? input.scope.environment.name : std::string{},
        .role = cameraRoleFromString(input.checkpoint.cameraRole)
                    .value_or(CameraRole::Other),
        .outdoor = input.checkpoint.outdoor,
        .zoneName = signals.zoneName,
        .reasons = reasonsFrom(input.checkpoint.reasons),
        .dwellS = std::max<int64_t>(0, signals.dwellMs) / 1000,
        .danger = input.danger,
        .action = action,
        .tamperStatus = {},
        .held = {},
        .routine = {},
        .notified = 0,
        .afterQuiet = false, .actorName = {}, .visitor = input.checkpoint.visitor};
    const NotifyContent content{
        .notice = notice,
        .data = noticeData(
            {.notice = notice,
             .environmentId = input.scope.environment.id,
             .digestDay = {},
             .rule = signals.rule,
             .incidentId = input.incidentId,
             .encounterId = input.encounterId,
             .zoneKind = signals.zoneKind,
             .identityState = identityStateToString(signals.identityState),
             .phase = "escalated"}),
        .excludeUserIds = {}};
    note(co_await performEffect(
        {.kind = GuardActionKind::Notify,
         .danger = input.danger,
         .cameraId = signals.cameraId,
         .incidentId = input.incidentId,
         .encounterId = input.encounterId,
         .personId = signals.personId,
         .now = input.now,
         .correlationId = correlation,
         .sequence = 1,
         .notifyContent = content}));
  }
  if (rank >= config->alarmLevel && deterrent.alarm) {
    note(co_await performEffect({.kind = GuardActionKind::Alarm,
                                 .danger = input.danger,
                                 .cameraId = signals.cameraId,
                                 .incidentId = input.incidentId,
                                 .encounterId = input.encounterId,
                                 .personId = signals.personId,
                                 .now = input.now,
                                 .seconds = config->alarmSeconds,
                                 .correlationId = correlation,
                                 .sequence = 3,
                                 .notifyContent = {}}));
    note(co_await performEffect({.kind = GuardActionKind::SirenArm,
                                 .danger = input.danger,
                                 .cameraId = signals.cameraId,
                                 .incidentId = input.incidentId,
                                 .encounterId = input.encounterId,
                                 .personId = signals.personId,
                                 .now = input.now,
                                 .correlationId = correlation,
                                 .sequence = 4,
                                 .notifyContent = {}}));
  }
  if (rank >= config->announceLevel && deterrent.voice) {
    const std::string text = sanitizeSpoken(config->announceText);
    if (!text.empty())
      note(co_await performEffect({.kind = GuardActionKind::Announce,
                                   .danger = input.danger,
                                   .cameraId = signals.cameraId,
                                   .incidentId = input.incidentId,
                                   .encounterId = input.encounterId,
                                   .personId = signals.personId,
                                   .now = input.now,
                                   .text = text,
                                   .lang = config->announceLang,
                                   .correlationId = correlation,
                                   .sequence = 2,
                                   .notifyContent = {}}));
  }
  co_return result;
}

drogon::Task<GuardService::DialogueResult>
GuardService::runDialogue(const DialogueInput& input)
{
  const auto config = currentConfig();
  DialogueResult result;
  if (!dependencies_.actions)
    co_return result;

  const auto encounter = co_await repository_.findEncounter(input.encounterId);
  const int turns = encounter ? encounter->dialogueTurns : 0;
  const std::string goal = encounter ? encounter->dialogueGoal : std::string{};
  if (goal == kGoalDone)
    co_return result;
  const bool canSpeakAgain = turns < std::max(1, config->maxDialogueTurns);

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
        greeting = config->greetKnownText.empty() ? config->greetText
                                                  : config->greetKnownText;
        const std::string placeholder = "{name}";
        if (const auto at = greeting.find(placeholder);
            at != std::string::npos) {
          greeting.replace(at, placeholder.size(), name);
        }
      }
      else {
        const std::vector<std::string> variants =
            config->greetTexts.empty()
                ? std::vector<std::string>{config->greetText}
                : config->greetTexts;
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
                                  .lang = config->greetLang,
                                  .correlationId = input.correlationId,
                                  .sequence = 1,
                                  .notifyContent = {}});
      result.greetingText = greeting;
      result.greetingStatus =
          !greet.authorized ? "denied" : (greet.accepted ? "sent" : "failed");
      result.greetingDetail = greet.detail;
      result.greeted = greet.accepted;
      if (!greet.accepted)
        LOG_WARN << "Guard dialogue: greet " << result.greetingStatus << ": "
                 << greet.detail;
      if (greet.resumable) {
        result.resumable = true;
        result.retryAt = greet.retryAt;
        co_return result;
      }
      if (!greet.accepted)
        co_return result;
      if (input.encounterId > 0) {
        co_await repository_.transitionEncounter(
            {.encounterId = input.encounterId,
             .toState = EncounterState::Challenging,
             .reason = "challenge",
             .grade = guard_policy::dangerToString(input.danger),
             .at = input.now});
        co_await repository_.setDialogueGoal(
            {.encounterId = input.encounterId,
             .goal = std::string(kGoalVerify),
             .listeningUntil = 0,
             .lastLine = greeting,
             .lastHeard = {}});
      }
    }
    else {
      result.greeted = true;
      result.greetingText = greeting;
    }

    if (config->greetListenSeconds > 0 && input.encounterId > 0) {
      if (goal.empty())
        co_await repository_.transitionEncounter(
            {.encounterId = input.encounterId,
             .toState = EncounterState::Listening,
             .reason = "listen",
             .grade = guard_policy::dangerToString(input.danger),
             .at = input.now});
      const int seconds = std::clamp(config->greetListenSeconds, 1, 10);
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
                                  .lang = config->greetLang,
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
      result.repairPending = unintelligible && !config->greetRepairText.empty();
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
    result.listenAfterReply = config->greetListenSeconds > 0;
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
        goal == kGoalRepair && !config->greetRepairText.empty();
    result.listenAfterReply = config->greetListenSeconds > 0;
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
  evidence["incidentId"] = static_cast<Json::Int64>(input.incidentId);
  evidence["encounterId"] = static_cast<Json::Int64>(input.encounterId);
  evidence["cameraId"] = static_cast<Json::Int64>(input.cameraId);
  evidence["cameraName"] = input.cameraName;
  evidence["rule"] = input.rule;
  evidence["danger"] = guardDangerToString(input.danger);
  evidence["event"] = stored;
  evidence["createdAt"] = static_cast<Json::Int64>(input.now);

  const bool serious =
      guardDangerRank(input.danger) >= guardDangerRank(GuardDanger::High);
  const std::string retentionClass =
      serious ? "extended"
              : (input.danger == GuardDanger::Medium ? "standard" : "short");
  const int64_t classS =
      serious ? 30LL * 24 * 3600
              : (input.danger == GuardDanger::Medium ? 7LL * 24 * 3600
                                                     : 24LL * 3600);
  const int64_t retentionS = std::min<int64_t>(
      classS, static_cast<int64_t>(currentConfig()->journalRetentionDays) * 86400);
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
  const auto config = currentConfig();
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (config->journalRetentionDays > 0) {
    const int64_t removed = co_await repository_.purgeDecisions(
        now - static_cast<int64_t>(config->journalRetentionDays) * 86400);
    if (removed > 0)
      LOG_INFO << "Guard retention: purged " << removed
               << " decision journal row(s)";
  }
  if (config->journalRetentionDays > 0) {
    const int64_t purged = co_await repository_.purgeHistory(
        {.historyBefore =
             now - static_cast<int64_t>(config->journalRetentionDays) * 86400,
         .inboxBefore = now - stream_retention::kRetentionSeconds});
    if (purged > 0)
      LOG_INFO << "Guard retention: purged " << purged
               << " history row(s) past the retention window";
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
  const auto config = currentConfig();
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
      result.unattended = response.get("unattended", false).asBool();
    }
    if (result.detail.empty())
      result.detail = !intent->detail.empty()
                          ? intent->detail
                          : guardIntentStatusToString(intent->status);
    co_return result;
  }

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
  const GuardActionAuthorizer authorizer(
      {.notifyLevel = config->notifyLevel,
       .announceLevel = config->announceLevel,
       .alarmLevel = config->alarmLevel,
       .armSiren = config->armSiren,
       .dialogueEnabled = config->greetEnabled});
  const GuardActionDecision decision =
      authorizer.authorize({.kind = input.kind,
                             .danger = input.danger,
                             .greetingEnabled = input.greetingEnabled,
                             .replyRequested = input.replyRequested});
  result.authorized = decision.authorized;
  result.detail = decision.reason;
  if (result.authorized && input.kind != GuardActionKind::Notify &&
      !dependencies_.actions) {
    result.authorized = false;
    result.detail = "camera_unavailable";
  }

  const bool budgeted = input.cameraId > 0 &&
                        (input.kind == GuardActionKind::Notify ||
                         input.kind == GuardActionKind::Announce ||
                         input.kind == GuardActionKind::Alarm ||
                         input.kind == GuardActionKind::SirenArm);
  if (result.authorized && budgeted && config->maxActionsPerHour > 0) {
    const int64_t used =
        co_await repository_.effectsSince(input.cameraId, input.now - 3600);
    if (used >= config->maxActionsPerHour) {
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
                             .content = input.notifyContent,
                             .cameraId = input.cameraId,
                             .danger = input.danger,
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
          case EffectStatus::NoRecipients:
            result.status = GuardIntentStatus::Rejected;
            result.unattended = true;
            result.detail = "no_recipients";
            break;
        }
        break;
      }
      case GuardActionKind::Greet:
      case GuardActionKind::Reply:
      case GuardActionKind::Announce: {
        const auto ack = co_await BlockingTask<CameraCommandResult>(
            [this, cameraId = input.cameraId, text = input.text,
             lang = input.lang, commandId, encounterId = input.encounterId]() {
              return dependencies_.actions->announce(
                  {.cameraId = cameraId,
                   .text = text,
                   .lang = lang,
                   .commandId = commandId,
                   .encounterId = encounterId,
                   .expiresAt = effectExpiry()});
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
             encounterId = input.encounterId]() {
              return dependencies_.actions->alarm({.cameraId = cameraId,
                                                   .seconds = seconds,
                                                   .commandId = commandId,
                                                   .encounterId = encounterId,
                                                   .expiresAt = effectExpiry()});
            });
        applyCamera(ack);
        break;
      }
      case GuardActionKind::SirenArm:
      case GuardActionKind::SirenDisarm: {
        const bool enabled = input.kind == GuardActionKind::SirenArm;
        const int leaseSeconds = enabled ? config->sirenSeconds : 0;
        const auto ack = co_await BlockingTask<CameraCommandResult>(
            [this, cameraId = input.cameraId, enabled, commandId,
             encounterId = input.encounterId, leaseSeconds]() {
              return dependencies_.actions->setSiren(
                  {.cameraId = cameraId,
                   .enabled = enabled,
                   .commandId = commandId,
                   .encounterId = encounterId,
                   .expiresAt = enabled ? effectExpiry() : 0,
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
  response["unattended"] = result.unattended;
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
  const auto config = currentConfig();
  if (config->sirenSeconds <= 0)
    return;
  EffectInput disarm = input;
  disarm.kind = GuardActionKind::SirenDisarm;
  disarm.text.clear();
  disarm.seconds = 0;
  trackTimer(drogon::app().getLoop()->runAfter(
      static_cast<double>(config->sirenSeconds),
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

drogon::Task<void> GuardService::maybeSendDigests(int64_t now)
{
  if (!evaluating())
    co_return;
  const auto config = currentConfig();
  const auto at = static_cast<std::time_t>(now);
  std::tm local{};
  if (localtime_r(&at, &local) == nullptr)
    co_return;
  std::vector<GuardEnvironment> environments;
  try {
    environments = co_await environmentRepository_.list();
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard service: digest sweep could not list environments: "
             << error.what();
    co_return;
  }
  const bool several = environments.size() > 1;
  for (const auto& environment : environments)
    co_await sendEnvironmentDigests(
        {.scope = {.environment = environment, .several = several},
         .config = *config,
         .local = local,
         .now = now});
}

drogon::Task<void>
GuardService::sendEnvironmentDigests(const EnvironmentDigestInput& input)
{
  const GuardEnvironment& environment = input.scope.environment;
  const std::tm& local = input.local;
  const int64_t now = input.now;
  const int64_t midnight =
      now - (local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
  const std::string today = std::to_string(local.tm_year + 1900) + "-" +
                            std::to_string(local.tm_mon + 1) + "-" +
                            std::to_string(local.tm_mday);
  std::array<char, 16> padded{};
  const size_t written = std::strftime(padded.data(), padded.size(), "%Y-%m-%d",
                                       &local);
  const std::string day(padded.data(), written);
  const std::string id = std::to_string(environment.id);
  const std::string quietKey = "digest_quiet_day_" + id;
  const std::string dailyKey = "digest_daily_day_" + id;
  const std::string untilKey = "digest_daily_until_" + id;
  const guard_schedule::QuietWindow window = guard_schedule::quietWindow(
      {.environment = environment, .config = input.config});
  if (window.enabled && local.tm_hour == window.endHour &&
      window.startHour != window.endHour &&
      co_await repository_.state(quietKey, "") != today) {
    const int64_t from =
        window.startHour > window.endHour
            ? midnight - static_cast<int64_t>(24 - window.startHour) * 3600
            : midnight + static_cast<int64_t>(window.startHour) * 3600;
    if (co_await sendDigest({.scope = input.scope,
                             .day = day,
                             .from = from,
                             .to = now,
                             .afterQuiet = true,
                             .correlationId = "digest:quiet:" + id + ":" + today,
                             .now = now}))
      co_await repository_.setState(
          {.key = quietKey, .value = today, .updatedAt = now});
  }
  if (environment.digestHour < 0 || local.tm_hour != environment.digestHour ||
      co_await repository_.state(dailyKey, "") == today)
    co_return;
  const std::string until = co_await repository_.state(untilKey, "");
  int64_t from = now - kDayS;
  if (const auto [end, error] =
          std::from_chars(until.data(), until.data() + until.size(), from);
      error != std::errc{} || end != until.data() + until.size())
    from = now - kDayS;
  if (window.enabled && co_await repository_.state(quietKey, "") == today)
    from = std::max<int64_t>(
        from, midnight + static_cast<int64_t>(window.endHour) * 3600);
  if (!co_await sendDigest({.scope = input.scope,
                            .day = day,
                            .from = std::max(from, now - kDigestLookbackS),
                            .to = now,
                            .afterQuiet = false,
                            .correlationId = "digest:daily:" + id + ":" + today,
                            .now = now}))
    co_return;
  co_await repository_.setState(
      {.key = dailyKey, .value = today, .updatedAt = now});
  co_await repository_.setState(
      {.key = untilKey, .value = std::to_string(now), .updatedAt = now});
}

drogon::Task<bool> GuardService::sendDigest(const DigestInput& input)
{
  const auto config = currentConfig();
  const GuardEnvironment& environment = input.scope.environment;
  const auto rows = co_await episodeRepository_.digest(
      {.from = input.from, .to = input.to, .environmentId = environment.id});
  std::vector<DigestLine> held;
  std::vector<DigestLine> routine;
  int64_t notified = 0;
  for (const auto& row : rows) {
    notified += row.notified;
    if (row.held > 0)
      held.push_back({.cameraId = row.cameraId,
                      .cameraName = row.cameraName,
                      .count = row.held});
    if (row.routine > 0)
      routine.push_back({.cameraId = row.cameraId,
                         .cameraName = row.cameraName,
                         .count = row.routine});
  }
  if (held.empty() && routine.empty())
    co_return true;
  const GuardNotice notice{.kind = NoticeKind::Digest,
                           .subject = NoticeSubject::Stranger,
                           .people = 0,
                           .cameraId = 0,
                           .cameraName = {},
                           .environmentName = input.scope.several
                                                  ? environment.name
                                                  : std::string{},
                           .role = CameraRole::Other,
                           .outdoor = false,
                           .zoneName = {},
                           .reasons = {},
                           .dwellS = 0,
                           .danger = GuardDanger::Low,
                           .action = NoticeAction::Watching,
                           .tamperStatus = {},
                           .held = std::move(held),
                           .routine = std::move(routine),
                           .notified = notified,
                           .afterQuiet = input.afterQuiet, .actorName = {}, .visitor = {}};
  Json::Value data = noticeData({.notice = notice,
                                 .environmentId = environment.id,
                                 .digestDay = input.day,
                                 .rule = "guard_digest",
                                 .incidentId = 0,
                                 .encounterId = 0,
                                 .zoneKind = {},
                                 .identityState = {},
                                 .phase = input.afterQuiet ? "after_quiet"
                                                           : "daily"});
  data["from"] = static_cast<Json::Int64>(input.from);
  data["to"] = static_cast<Json::Int64>(input.to);
  data["day"] = input.day;
  const GuardDanger authorized =
      guardDangerFromRank(std::clamp(config->notifyLevel, 1, 4));
  const EffectResult sent = co_await performEffect(
      {.kind = GuardActionKind::Notify,
       .danger = authorized,
       .greetingEnabled = false,
       .replyRequested = false,
       .cameraId = 0,
       .incidentId = 0,
       .encounterId = 0,
       .personId = 0,
       .now = input.now,
       .text = {},
       .lang = {},
       .seconds = 0,
       .correlationId = input.correlationId,
       .sequence = 1,
       .notifyContent = {.notice = notice, .data = data, .excludeUserIds = {}}});
  co_return !sent.resumable;
}

std::string GuardService::userLang(int64_t userId)
{
  const auto config = currentConfig();
  const auto now = static_cast<int64_t>(std::time(nullptr));
  {
    std::scoped_lock lock(langMutex_);
    const auto found = langCache_.find(userId);
    if (found != langCache_.end() &&
        now - found->second.resolvedAt < kLangCacheSeconds)
      return found->second.lang;
  }
  const auto user = dependencies_.identity
                        ? dependencies_.identity->getUser(userId)
                        : std::nullopt;
  const std::string lang = guard_copy::normalizeLang(
      {.requested = user ? languageOf(*user) : std::string{},
       .fallback = config->notifyLang});
  if (user) {
    std::scoped_lock lock(langMutex_);
    langCache_[userId] = {.lang = lang, .resolvedAt = now};
  }
  return lang;
}

drogon::Task<GuardService::RecipientResolution>
GuardService::legacyRecipients(const RecipientQuery& query)
{
  if (!dependencies_.identity) {
    LOG_WARN << "Guard service: identity SDK unavailable";
    co_return RecipientResolution{
        .outcome = RecipientOutcome::Unavailable, .batches = {}, .plan = Json::Value()};
  }
  const bool redacted = visitorPresent(query.data);
  co_return co_await BlockingTask<RecipientResolution>(
      [this, excluded = query.excluded, redacted]() -> RecipientResolution {
        const auto users = dependencies_.identity->listNotifiableUsers();
        if (!users || users->empty())
          return RecipientResolution{.outcome = RecipientOutcome::Unavailable,
                                     .batches = {},
                                     .plan = Json::Value()};
        std::map<std::string, std::vector<int64_t>> byLang;
        for (const int64_t userId : *users) {
          if (std::ranges::find(excluded, userId) == excluded.end())
            byLang[userLang(userId)].push_back(userId);
        }
        RecipientResolution resolution;
        if (byLang.empty()) {
          resolution.outcome = RecipientOutcome::Empty;
          return resolution;
        }
        resolution.outcome = RecipientOutcome::Planned;
        resolution.batches.reserve(byLang.size());
        for (auto& [lang, ids] : byLang)
          resolution.batches.push_back(
              {.lang = lang, .userIds = std::move(ids), .redacted = redacted});
        return resolution;
      });
}

namespace
{
std::optional<ResponseTrigger> responseTriggerOf(const Json::Value& data)
{
  const std::string kind = data.get("kind", "").asString();
  const std::string urgency = data.get("urgency", "").asString();
  const bool critical = urgency == "critical";
  if (kind == "guard_episode" && (critical || urgency == "time_sensitive"))
    return data.get("phase", "").asString() == "escalated"
               ? ResponseTrigger::Escalation
               : ResponseTrigger::Intrusion;
  if (kind == "guard_panic" && critical)
    return ResponseTrigger::Panic;
  if (kind == "guard_duress" && critical)
    return ResponseTrigger::Duress;
  if (kind == "guard_tamper" && critical)
    return ResponseTrigger::Tamper;
  return std::nullopt;
}

bool hasReason(const Json::Value& data, std::string_view reason)
{
  const Json::Value& reasons = data["reasons"];
  if (!reasons.isArray())
    return false;
  return std::ranges::any_of(reasons, [reason](const Json::Value& value) {
    return value.isString() && value.asString() == reason;
  });
}
}

drogon::Task<GuardService::RecipientResolution>
GuardService::planRecipients(const PlanQuery& query)
{
  const auto config = currentConfig();
  const Json::Value& data = query.data;
  int64_t environmentId = data.get("environmentId", 0).asInt64();
  std::optional<GuardEnvironment> environment;
  if (environmentId > 0)
    environment = co_await environmentRepository_.find(environmentId);
  if (!environment) {
    environmentId = co_await environmentRepository_.defaultId();
    if (environmentId > 0)
      environment = co_await environmentRepository_.find(environmentId);
  }
  const ResponseConfigRows rows = environmentId > 0
                                      ? co_await responseRepository_.forEnvironment(
                                            environmentId)
                                      : ResponseConfigRows{};
  const std::vector<ResponseMember> members = response_plan::members(
      {.users = query.users, .rows = rows.recipients});
  const auto langOf = [&](int64_t userId) {
    const auto user = std::ranges::find(query.users, userId, &ResponseUser::userId);
    return guard_copy::normalizeLang(
        {.requested = user != query.users.end() ? user->lang : std::string{},
         .fallback = config->notifyLang});
  };
  const auto redactedOf = [&](int64_t userId) {
    const auto user = std::ranges::find(query.users, userId, &ResponseUser::userId);
    return visitorRedactedFor(data,
                              user != query.users.end() ? user->role : UserRole::Unknown);
  };
  const auto batchesOf = [&](const std::vector<int64_t>& userIds) {
    std::map<std::pair<std::string, bool>, std::vector<int64_t>> byAudience;
    for (const int64_t userId : userIds)
      byAudience[{langOf(userId), redactedOf(userId)}].push_back(userId);
    std::vector<RecipientBatch> batches;
    batches.reserve(byAudience.size());
    for (auto& [audience, ids] : byAudience)
      batches.push_back(
          {.lang = audience.first, .userIds = std::move(ids), .redacted = audience.second});
    return batches;
  };

  const auto trigger = responseTriggerOf(data);
  if (!trigger) {
    std::vector<RecipientBatch> batches = batchesOf(response_plan::audience(
        {.members = members, .excluded = query.excluded}));
    co_return RecipientResolution{
        .outcome = batches.empty() ? RecipientOutcome::Empty : RecipientOutcome::Planned,
        .batches = std::move(batches),
        .plan = Json::Value()};
  }

  std::vector<int64_t> memberIds;
  memberIds.reserve(members.size());
  for (const auto& member : members)
    memberIds.push_back(member.userId);
  std::vector<PresenceRow> presence;
  if (environmentId > 0 && !memberIds.empty()) {
    try {
      presence = co_await presenceRepository_.forEnvironment(
          {.environmentId = environmentId, .userIds = memberIds});
    }
    catch (const std::exception& error) {
      LOG_WARN << "Guard service: presence read failed, treating as unknown: "
               << error.what();
    }
  }
  const auto at = static_cast<std::time_t>(query.now);
  std::tm local{};
  localtime_r(&at, &local);
  const GuardSchedule schedule = environment
                                     ? guard_schedule::fromEnvironment(*environment)
                                     : GuardSchedule{};
  const GuardPosture posture =
      guard_schedule::resolve({.schedule = schedule,
                               .manual = environment ? environment->mode
                                                     : GuardMode::Home,
                               .local = local});
  const int64_t cameraId = data.get("cameraId", 0).asInt64();
  bool indoor = false;
  if (cameraId > 0) {
    const GuardCameraContext camera = co_await cameraContextRepository_.find(cameraId);
    indoor = camera.configured && !camera.outdoor;
  }
  const ResponsePlan plan = response_plan::build(
      {.environmentId = environmentId,
       .members = members,
       .presence = presence,
       .excluded = query.excluded,
       .trigger = *trigger,
       .indoor = indoor,
       .night = posture.mode == GuardMode::Night || hasReason(data, "night"),
       .clearThreat = response_plan::clearThreat(data),
       .staffedNow = response_plan::staffedAt(schedule, local),
       .hasCamera = cameraId > 0,
       .stepSeconds = rows.setting.stepSeconds,
       .emergencyNumber = rows.setting.emergencyNumber,
       .contacts = rows.contacts});
  std::vector<RecipientBatch> batches =
      batchesOf(response_plan::stepUsers(plan, 0));
  co_return RecipientResolution{
      .outcome = batches.empty() ? RecipientOutcome::Empty : RecipientOutcome::Planned,
      .batches = std::move(batches),
      .plan = response_plan::toJson(plan)};
}

drogon::Task<std::optional<std::vector<ResponseUser>>> GuardService::directoryUsers()
{
  const auto directory = dependencies_.directory;
  auto users = co_await BlockingTask<std::optional<std::vector<ResponseUser>>>(
      [directory]() { return directory->users(); });
  if (users && !users->empty()) {
    const std::string snapshot = directorySnapshot(*users);
    bool changed = false;
    {
      std::scoped_lock lock(directoryMutex_);
      changed = !lastDirectory_ || directorySnapshot(*lastDirectory_) != snapshot;
      lastDirectory_ = *users;
    }
    if (changed) {
      try {
        co_await repository_.setState({.key = std::string(kDirectorySnapshotKey),
                                       .value = snapshot,
                                       .updatedAt = static_cast<int64_t>(std::time(nullptr))});
      }
      catch (const std::exception& error) {
        LOG_WARN << "Guard service: directory snapshot not stored: " << error.what();
      }
    }
    co_return users;
  }
  {
    std::scoped_lock lock(directoryMutex_);
    if (lastDirectory_)
      co_return lastDirectory_;
  }
  try {
    co_return directoryFromSnapshot(
        co_await repository_.state(kDirectorySnapshotKey, ""));
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard service: directory snapshot unreadable: " << error.what();
  }
  co_return std::nullopt;
}

drogon::Task<GuardService::RecipientResolution>
GuardService::recipientsFor(const RecipientQuery& query)
{
  if (!dependencies_.directory)
    co_return co_await legacyRecipients(query);
  const auto users = co_await directoryUsers();
  if (!users) {
    LOG_WARN << "Guard service: user directory unavailable; using the fallback roster";
    co_return co_await legacyRecipients(query);
  }
  co_return co_await planRecipients(
      {.data = query.data, .excluded = query.excluded, .users = *users, .now = query.now});
}

drogon::Task<bool> GuardService::familyInside(int64_t environmentId)
{
  if (environmentId <= 0)
    co_return false;
  try {
    const auto rows = co_await presenceRepository_.forEnvironment(
        {.environmentId = environmentId, .userIds = {}});
    co_return presence::overall(rows) == PresenceState::Home;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Guard service: presence read failed: " << error.what();
  }
  co_return false;
}

drogon::Task<GuardService::EmptyAudience>
GuardService::emptyAudience(const NotifyInput& input,
                            const RecipientResolution& resolution)
{
  const int64_t actorUserId = input.content.excludeUserIds.empty()
                                  ? 0
                                  : input.content.excludeUserIds.front();
  const bool panic = input.content.notice.kind == NoticeKind::Panic;
  std::vector<GuardContact> contacts;
  if (resolution.plan["contacts"].isArray()) {
    for (const auto& contact : resolution.plan["contacts"])
      contacts.push_back({.name = contact.get("name", "").asString(),
                          .phone = contact.get("phone", "").asString()});
  }
  if (panic && actorUserId > 0 && !contacts.empty()) {
    const std::string lang = co_await BlockingTask<std::string>(
        [this, actorUserId]() { return userLang(actorUserId); });
    co_return EmptyAudience{.status = EffectStatus::Succeeded,
                            .batches = {{.lang = lang,
                                         .userIds = {actorUserId},
                                         .redacted = false}},
                            .contacts = std::move(contacts)};
  }
  if (panic && actorUserId > 0) {
    if (!co_await notifyUnattended(input, actorUserId))
      co_return EmptyAudience{
          .status = EffectStatus::RetryableFailed, .batches = {}, .contacts = {}};
  }
  LOG_ERROR << "Guard service: nobody can be alerted by policy; notification "
            << input.commandId << " is not retried";
  co_return EmptyAudience{};
}

drogon::Task<bool>
GuardService::notifyUnattended(const NotifyInput& input, int64_t actorUserId)
{
  if (!dependencies_.notifications)
    co_return false;
  const int64_t alertId = input.content.data.get("alertId", 0).asInt64();
  const std::string reference =
      alertId > 0 ? std::to_string(alertId) : input.eventId;
  const std::string lang = co_await BlockingTask<std::string>(
      [this, actorUserId]() { return userLang(actorUserId); });
  const NoticeText text = guard_copy::unattended(lang);
  Json::Value data(Json::objectValue);
  data["kind"] = "guard_panic_unattended";
  data["urgency"] = "critical";
  data["silent"] = true;
  data["threadKey"] = "guard:panic:" + reference;
  data["alertId"] = static_cast<Json::Int64>(alertId);
  data["cameraId"] = 0;
  data["lang"] = lang;
  const auto result = co_await BlockingTask<NotificationCreateResult>(
      [this, actorUserId, reference, text, data]() {
        argus::notification::v1::CreateNotificationsRequest request;
        request.add_user_ids(actorUserId);
        request.set_command_id("panic:" + reference + ":unattended");
        request.set_type("camera");
        request.set_title(text.title);
        request.set_body(text.body);
        request.set_data(json_util::toString(data));
        return dependencies_.notifications->createNotifications(
            request, {.userId = 0, .role = "system", .device = "argus-guard"});
      });
  if (result.outcome != NotificationRpcOutcome::Success) {
    LOG_WARN << "Guard service: the unattended-panic notice for alert "
             << reference << " was not delivered";
    co_return false;
  }
  co_return true;
}

drogon::Task<GuardService::EffectStatus>
GuardService::notify(const NotifyInput& input)
{
  const auto config = currentConfig();
  if (!dependencies_.notifications) {
    LOG_WARN << "Guard service: notification SDK unavailable";
    co_return EffectStatus::RetryableFailed;
  }

  struct Delivery
  {
    std::string lang;
    std::vector<int64_t> userIds;
    std::string title;
    std::string body;
    bool redacted{false};
  };
  std::vector<Delivery> deliveries;
  Json::Value data;
  if (!input.payload.empty()) {
    const Json::Value stored = json_util::fromString(input.payload);
    if (stored.isObject()) {
      data = stored["data"];
      const auto readIds = [](const Json::Value& ids) {
        std::vector<int64_t> userIds;
        for (const auto& userId : ids)
          userIds.push_back(userId.asInt64());
        return userIds;
      };
      if (stored["batches"].isArray()) {
        for (const auto& batch : stored["batches"])
          deliveries.push_back({.lang = batch.get("lang", "").asString(),
                                .userIds = readIds(batch["userIds"]),
                                .title = batch.get("title", "").asString(),
                                .body = batch.get("body", "").asString(),
                                .redacted = batch.get("redacted", false).asBool()});
      }
      else if (stored["userIds"].isArray()) {
        deliveries.push_back({.lang = {},
                              .userIds = readIds(stored["userIds"]),
                              .title = stored.get("title", "").asString(),
                              .body = stored.get("body", "").asString(),
                              .redacted = false});
      }
    }
    const bool usable =
        !deliveries.empty() &&
        std::ranges::all_of(deliveries, [](const Delivery& delivery) {
          return !delivery.userIds.empty();
        });
    if (!usable) {
      LOG_ERROR << "Guard service: persisted notification payload unusable";
      co_return EffectStatus::RetryableFailed;
    }
  }

  if (deliveries.empty()) {
    const RecipientResolution resolution = co_await recipientsFor(
        {.data = input.content.data,
         .excluded = input.content.excludeUserIds,
         .now = input.now});
    if (resolution.outcome == RecipientOutcome::Unavailable) {
      LOG_WARN << "Guard service: no notifiable users; notification dropped";
      co_return EffectStatus::RetryableFailed;
    }
    std::vector<RecipientBatch> batches = resolution.batches;
    std::vector<GuardContact> contacts;
    if (resolution.outcome == RecipientOutcome::Empty) {
      EmptyAudience fallback = co_await emptyAudience(input, resolution);
      if (fallback.status != EffectStatus::Succeeded)
        co_return fallback.status;
      batches = std::move(fallback.batches);
      contacts = std::move(fallback.contacts);
    }
    data = input.content.data;
    if (resolution.plan.isObject() && data.isObject())
      data["response"] = resolution.plan;
    Json::Value payload(Json::objectValue);
    payload["type"] = "camera";
    payload["data"] = data;
    Json::Value storedBatches(Json::arrayValue);
    for (const auto& batch : batches) {
      GuardNotice notice = input.content.notice;
      if (batch.redacted)
        notice.visitor = {};
      const NoticeText text =
          contacts.empty()
              ? guard_copy::render(notice, batch.lang)
              : guard_copy::panicContacts(
                    {.notice = notice, .contacts = contacts, .lang = batch.lang});
      deliveries.push_back({.lang = batch.lang,
                            .userIds = batch.userIds,
                            .title = text.title,
                            .body = text.body,
                            .redacted = batch.redacted});
      Json::Value stored(Json::objectValue);
      stored["lang"] = batch.lang;
      stored["redacted"] = batch.redacted;
      Json::Value ids(Json::arrayValue);
      for (const int64_t userId : batch.userIds)
        ids.append(static_cast<Json::Int64>(userId));
      stored["userIds"] = std::move(ids);
      stored["title"] = text.title;
      stored["body"] = text.body;
      storedBatches.append(std::move(stored));
    }
    payload["batches"] = std::move(storedBatches);
    bool persisted = false;
    if (config->failPoint && config->failPoint("notify_persist_fail"))
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

  if (!input.eventId.empty())
    co_await repository_.bumpDispatchAttempts(input.eventId);

  int32_t created = 0;
  int64_t expected = 0;
  bool replayed = false;
  for (const auto& delivery : deliveries) {
    argus::notification::v1::CreateNotificationsRequest request;
    for (const int64_t userId : delivery.userIds)
      request.add_user_ids(userId);
    request.set_command_id(deliveries.size() == 1
                               ? input.commandId
                               : input.commandId + ":" + delivery.lang +
                                     (delivery.redacted ? ":r" : ""));
    request.set_type("camera");
    request.set_title(delivery.title);
    request.set_body(delivery.body);
    Json::Value localized = data;
    if (localized.isObject() && !delivery.lang.empty())
      localized["lang"] = delivery.lang;
    if (localized.isObject() && delivery.redacted)
      localized.removeMember("visitor");
    request.set_data(json_util::toString(localized));
    const auto result =
        co_await BlockingTask<NotificationCreateResult>([this, request]() {
          return dependencies_.notifications
              ->createNotifications(request, {.userId = 0,
                                              .role = "system",
                                              .device = "argus-guard"});
        });
    if (result.outcome == NotificationRpcOutcome::Conflict) {
      LOG_ERROR << "Guard service: notification command conflict for "
                << request.command_id();
      co_return EffectStatus::Rejected;
    }
    if (result.outcome == NotificationRpcOutcome::Rejected) {
      LOG_ERROR << "Guard service: notification rejected: "
                << result.status.error_message() << " for "
                << request.command_id();
      co_return EffectStatus::Rejected;
    }
    if (result.outcome == NotificationRpcOutcome::Unavailable) {
      LOG_WARN << "Guard service: notification service unavailable (camera "
               << input.cameraId << ")";
      co_return EffectStatus::RetryableFailed;
    }
    if (result.created != static_cast<int32_t>(delivery.userIds.size())) {
      LOG_WARN << "Guard service: notification batch incomplete ("
               << result.created << " of " << delivery.userIds.size()
               << "); will retry";
      co_return EffectStatus::RetryableFailed;
    }
    created += result.created;
    expected += static_cast<int64_t>(delivery.userIds.size());
    replayed = replayed || result.duplicate;
  }
  LOG_INFO << "Guard service: notified " << created << " of " << expected
           << " user(s)" << (replayed ? " (replayed)" : "") << " for camera "
           << input.cameraId << " (danger " << guardDangerToString(input.danger)
           << ")";
  if (!input.eventId.empty())
    co_await repository_.recordNotificationDispatch(
        {.eventId = input.eventId,
         .encounterId = input.encounterId,
         .commandId = input.commandId,
         .rank = guard_policy::dangerRank(input.danger),
         .failPoint = config->failPoint});
  co_return EffectStatus::Succeeded;
}

drogon::Task<SafetyReach> GuardService::safetyReach(int64_t actorUserId)
{
  Json::Value data(Json::objectValue);
  data["kind"] = noticeKindToString(NoticeKind::Panic);
  data["urgency"] = "critical";
  data["environmentId"] = 0;
  const std::vector<int64_t> excluded{actorUserId};
  const RecipientResolution resolution = co_await recipientsFor(
      {.data = data, .excluded = excluded, .now = static_cast<int64_t>(std::time(nullptr))});
  switch (resolution.outcome) {
    case RecipientOutcome::Planned:
      co_return SafetyReach::Others;
    case RecipientOutcome::Unavailable:
      co_return SafetyReach::Unknown;
    case RecipientOutcome::Empty:
      break;
  }
  const Json::Value& contacts = resolution.plan["contacts"];
  co_return contacts.isArray() && !contacts.empty() ? SafetyReach::Contacts : SafetyReach::Nobody;
}

drogon::Task<GuardService::SafetyAlertOutcome>
GuardService::raiseSafetyAlert(const SafetyAlertInput& input)
{
  const int64_t environmentId = input.environmentId > 0
                                    ? input.environmentId
                                    : co_await environmentRepository_.defaultId();
  const auto environment = co_await environmentRepository_.find(environmentId);
  const bool several = co_await environmentRepository_.count() > 1;
  const NoticeKind kind = !input.duress ? NoticeKind::Panic
                          : input.spoken ? NoticeKind::SpokenDuress
                                         : NoticeKind::Duress;
  const GuardNotice notice{.kind = kind,
                           .subject = NoticeSubject::Stranger,
                           .people = 0,
                           .cameraId = 0,
                           .cameraName = {},
                           .environmentName = several && environment ? environment->name
                                                                     : std::string{},
                           .role = CameraRole::Other,
                           .outdoor = false,
                           .zoneName = {},
                           .reasons = {},
                           .dwellS = 0,
                           .danger = GuardDanger::Critical,
                           .action = NoticeAction::Watching,
                           .tamperStatus = {},
                           .held = {},
                           .routine = {},
                           .notified = 0,
                           .afterQuiet = false,
                           .actorName = input.actorName,
                           .visitor = {}};
  Json::Value data = noticeData({.notice = notice,
                                 .environmentId = environment ? environment->id : 0,
                                 .digestDay = {},
                                 .rule = input.duress ? "duress" : "panic",
                                 .incidentId = 0,
                                 .encounterId = input.alertId,
                                 .zoneKind = {},
                                 .identityState = {},
                                 .phase = "opened"});
  data["encounterId"] = 0;
  data["alertId"] = static_cast<Json::Int64>(input.alertId);
  data["actorUserId"] = static_cast<Json::Int64>(input.actorUserId);
  data["actorName"] = input.actorName;
  data["saidAt"] = input.saidAt > 0 ? Json::Value(static_cast<Json::Int64>(input.saidAt)) : Json::Value();
  data["deliveredAt"] = static_cast<Json::Int64>(input.now);
  const EffectResult sent = co_await performEffect(
      {.kind = GuardActionKind::Notify,
       .danger = GuardDanger::Critical,
       .greetingEnabled = false,
       .replyRequested = false,
       .cameraId = 0,
       .incidentId = 0,
       .encounterId = 0,
       .personId = 0,
       .now = input.now,
       .text = {},
       .lang = {},
       .seconds = 0,
       .correlationId = "safety:" + std::to_string(input.alertId) +
                        (input.sequence > 1 ? ":r" + std::to_string(input.sequence)
                                            : std::string{}),
       .sequence = 1,
       .notifyContent = {.notice = notice,
                         .data = std::move(data),
                         .excludeUserIds = {input.actorUserId}}});
  co_return SafetyAlertOutcome{
      .accepted = sent.accepted,
      .terminal = !sent.accepted && guardIntentStatusIsTerminal(sent.status),
      .unattended = sent.unattended};
}
