#include <sync/camera-notifier.hxx>

#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

#include <ctime>
#include <json/value.h>
#include <vector>

namespace
{
constexpr int kHourMs = 60 * 60 * 1000;

int hourOfDay(int64_t nowMs)
{
  const std::time_t tick = static_cast<std::time_t>(nowMs / 1000);
  std::tm local{};
  if (!localtime_r(&tick, &local))
    return 0;
  return local.tm_hour;
}

std::string titleFor(const Json::Value& event)
{
  const std::string camera = event.get("cameraName", "").asString();
  const std::string rule = event.get("rule", "").asString();
  return (camera.empty() ? "Camera" : camera) + ": " + rule;
}

std::string bodyFor(const Json::Value& event)
{
  const std::string severity = event.get("severity", "info").asString();
  std::string classes;
  for (const auto& object : event.get("objects", Json::Value())) {
    const std::string name = object.get("class", "").asString();
    if (!name.empty() && classes.find(name) == std::string::npos)
      classes += (classes.empty() ? "" : ", ") + name;
  }
  return "Severity " + severity +
         (classes.empty() ? "" : "; detected " + classes);
}

// Raw fallback only carries protected-zone hard signals while guard is absent.
bool isHardSignal(const Json::Value& event)
{
  if (event.get("severity", "").asString() == "critical")
    return true;
  const std::string rule = event.get("rule", "").asString();
  return rule == "person_in_alert_zone";
}

struct FallbackSignals
{
  std::string identityState;
  double scoreMedian{0.0};
  int scoreSamples{0};
  int64_t dwellMs{0};
  bool hasScore{false};
  bool hasDwell{false};
};

FallbackSignals fallbackSignals(const Json::Value& event)
{
  FallbackSignals signals;
  const Json::Value& objects = event["objects"];
  const int64_t primaryTrackId = event.get("trackId", 0).asInt64();
  const Json::Value* primary = nullptr;
  const Json::Value* largest = nullptr;
  double largestArea = -1.0;
  if (objects.isArray()) {
    for (const auto& object : objects) {
      if (object.get("class", "").asString() != "person")
        continue;
      const double area = object["bbox"].get("w", 0.0).asDouble() *
                          object["bbox"].get("h", 0.0).asDouble();
      if (area > largestArea) {
        largestArea = area;
        largest = &object;
      }
      if (primaryTrackId > 0 &&
          object.get("trackId", 0).asInt64() == primaryTrackId)
        primary = &object;
    }
  }
  if (primary == nullptr)
    primary = largest;
  if (primary != nullptr) {
    signals.identityState = primary->get("identityState", "").asString();
    if (primary->isMember("scoreMedian")) {
      signals.scoreMedian = primary->get("scoreMedian", 0.0).asDouble();
      signals.scoreSamples = primary->get("scoreSamples", 0).asInt();
      signals.hasScore = true;
    }
    if (primary->isMember("dwellMs")) {
      signals.dwellMs = primary->get("dwellMs", 0).asInt64();
      signals.hasDwell = true;
    }
  }
  if (!signals.hasDwell && event.isMember("dwellMs")) {
    signals.dwellMs = event.get("dwellMs", 0).asInt64();
    signals.hasDwell = true;
  }
  return signals;
}

const char* fallbackReason(
    CameraNotificationPolicy::FallbackDecision decision)
{
  switch (decision) {
  case CameraNotificationPolicy::FallbackDecision::Notify:
    return "pass";
  case CameraNotificationPolicy::FallbackDecision::DropKnown:
    return "known identity";
  case CameraNotificationPolicy::FallbackDecision::DropWeakScore:
    return "weak detector score";
  case CameraNotificationPolicy::FallbackDecision::DropShortDwell:
    return "short dwell";
  }
  return "pass";
}
} // namespace

CameraNotificationPolicy::CameraNotificationPolicy(Config config)
    : config_(config)
{
}

bool CameraNotificationPolicy::inSilentHours(const Config& config, int hour)
{
  const int start = config.silentStartHour;
  const int end = config.silentEndHour;
  if (start < 0 || end < 0 || start == end)
    return false;
  return start < end ? (hour >= start && hour < end)
                     : (hour >= start || hour < end);
}

bool CameraNotificationPolicy::guardReady(int64_t nowMs) const
{
  return lastGuardHeartbeatMs_ > 0 &&
         nowMs - lastGuardHeartbeatMs_ <= config_.guardTimeoutMs;
}

void CameraNotificationPolicy::markGuardHeartbeat(int64_t nowMs)
{
  lastGuardHeartbeatMs_ = nowMs;
}

CameraNotificationPolicy::FallbackDecision
CameraNotificationPolicy::fallbackDecision(const Json::Value& event) const
{
  const FallbackSignals signals = fallbackSignals(event);
  if (config_.fallbackSuppressKnown && signals.identityState == "known")
    return FallbackDecision::DropKnown;
  if (signals.hasScore && signals.scoreMedian < config_.fallbackMinScoreMedian)
    return FallbackDecision::DropWeakScore;
  if (signals.hasDwell && signals.dwellMs < config_.fallbackMinDwellMs)
    return FallbackDecision::DropShortDwell;
  return FallbackDecision::Notify;
}

CameraNotificationPolicy::FallbackCounts
CameraNotificationPolicy::fallbackCounts() const
{
  return {.passed = fallbackPassed_.load(),
          .droppedKnown = fallbackDroppedKnown_.load(),
          .droppedWeakScore = fallbackDroppedWeakScore_.load(),
          .droppedShortDwell = fallbackDroppedShortDwell_.load()};
}

void CameraNotificationPolicy::countFallbackPass()
{
  fallbackPassed_.fetch_add(1);
}

void CameraNotificationPolicy::countFallbackDrop(FallbackDecision decision)
{
  switch (decision) {
  case FallbackDecision::DropKnown:
    fallbackDroppedKnown_.fetch_add(1);
    break;
  case FallbackDecision::DropWeakScore:
    fallbackDroppedWeakScore_.fetch_add(1);
    break;
  case FallbackDecision::DropShortDwell:
    fallbackDroppedShortDwell_.fetch_add(1);
    break;
  case FallbackDecision::Notify:
    break;
  }
}

bool CameraNotificationPolicy::shouldNotify(int64_t cameraId, int64_t nowMs)
{
  auto& state = windows_[cameraId];
  if (state.windowStartMs == 0 || nowMs - state.windowStartMs >= kHourMs) {
    // The roll marks a pending digest due instead of clearing the counts.
    if (state.windowStartMs != 0 && !state.suppressedByClass.empty())
      state.digestDue = true;
    state.windowStartMs = nowMs;
    state.notified = 0;
  }

  if (inSilentHours(config_, hourOfDay(nowMs)))
    return false;

  if (state.notified >= config_.budgetPerHour)
    return false;

  ++state.notified;
  return true;
}

void CameraNotificationPolicy::countSuppressed(int64_t cameraId,
                                               const std::string& objectClass)
{
  ++windows_[cameraId].suppressedByClass[objectClass];
}

std::vector<int64_t>
CameraNotificationPolicy::trackedCameras() const
{
  std::vector<int64_t> ids;
  ids.reserve(windows_.size());
  for (const auto& [cameraId, state] : windows_)
    ids.push_back(cameraId);
  return ids;
}

std::string CameraNotificationPolicy::takeDigest(int64_t cameraId,
                                                 int64_t nowMs)
{
  auto it = windows_.find(cameraId);
  if (it == windows_.end() || it->second.suppressedByClass.empty())
    return {};

  auto& state = it->second;
  // The counts carry into the next active window; nothing delivers in silent hours.
  if (inSilentHours(config_, hourOfDay(nowMs)))
    return {};

  const bool silentOver =
      inSilentHours(config_, hourOfDay(nowMs - 1));
  const bool hourRolled =
      state.digestDue ||
      (state.windowStartMs != 0 && nowMs - state.windowStartMs >= kHourMs);
  if (!silentOver && !hourRolled)
    return {};

  std::string summary;
  int total = 0;
  for (const auto& [objectClass, count] : state.suppressedByClass) {
    total += count;
    summary += (summary.empty() ? "" : ", ") + std::to_string(count) + " " +
               objectClass;
  }
  state.suppressedByClass.clear();
  state.digestDue = false;
  return std::to_string(total) + " events suppressed (" + summary + ")";
}

CameraObjectNotifier::CameraObjectNotifier(
    CameraNotificationPolicy::Config config,
    std::shared_ptr<NotificationClient> client)
    : notificationClient_(std::move(client)), policy_(config)
{
}

void CameraObjectNotifier::handle(const Json::Value& json)
{
  if (!json.isObject()) {
    LOG_WARN << "Camera notifier: dropped malformed object_detected event";
    return;
  }

  const auto cameraId = json.get("cameraId", 0).asInt64();
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

  if (policy_.guardReady(nowMs)) {
    LOG_INFO << "Camera notifier: guard owns notifications; raw camera "
             << cameraId << " event suppressed";
    return;
  }
  if (!isHardSignal(json)) {
    LOG_INFO << "Camera notifier: guard absent; non-hard camera " << cameraId
             << " event ignored by the fallback";
    logFallback({.cameraId = cameraId,
                 .rule = json.get("rule", "").asString(),
                 .severity = json.get("severity", "").asString(),
                 .reason = FallbackDropReason::NonHardSignal,
                 .createdAt = nowMs / 1000});
    return;
  }
  const auto fallback = policy_.fallbackDecision(json);
  if (fallback != CameraNotificationPolicy::FallbackDecision::Notify) {
    for (const auto& object : json.get("objects", Json::Value()))
      policy_.countSuppressed(cameraId, object.get("class", "").asString());
    policy_.countFallbackDrop(fallback);
    LOG_INFO << "Camera notifier: fallback gate dropped camera " << cameraId
             << " event (" << fallbackReason(fallback) << ")";
    logFallback({.cameraId = cameraId,
                 .rule = json.get("rule", "").asString(),
                 .severity = json.get("severity", "").asString(),
                 .reason = fallback == CameraNotificationPolicy::
                                           FallbackDecision::DropKnown
                               ? FallbackDropReason::DropKnown
                           : fallback == CameraNotificationPolicy::
                                             FallbackDecision::DropWeakScore
                               ? FallbackDropReason::DropWeakScore
                               : FallbackDropReason::DropShortDwell,
                 .createdAt = nowMs / 1000});
    return;
  }
  policy_.countFallbackPass();

  if (!policy_.shouldNotify(cameraId, nowMs)) {
    for (const auto& object : json.get("objects", Json::Value()))
      policy_.countSuppressed(cameraId, object.get("class", "").asString());
    LOG_INFO << "Camera notifier: budget or silent hours suppressed camera "
             << cameraId;
    logFallback({.cameraId = cameraId,
                 .rule = json.get("rule", "").asString(),
                 .severity = json.get("severity", "").asString(),
                 .reason = FallbackDropReason::BudgetSilent,
                 .createdAt = nowMs / 1000});
    return;
  }

  deliver({.json = json, .title = titleFor(json), .body = bodyFor(json)});
}

void CameraObjectNotifier::logFallback(const FallbackLogInput& input)
{
  if (!DbService::gatewayClient())
    return;
  drogon::async_run([this, input]() -> drogon::Task<void> {
    try {
      co_await fallbackLogRepository_.log(input);
    }
    catch (...) {
    }
    co_return;
  });
}

void CameraObjectNotifier::purgeFallbackLog(int64_t nowS)
{
  if (policy_.config().fallbackRetentionDays <= 0)
    return;
  const int64_t cutoff =
      nowS - static_cast<int64_t>(policy_.config().fallbackRetentionDays) *
                 86400;
  drogon::async_run([this, cutoff]() -> drogon::Task<void> {
    try {
      co_await fallbackLogRepository_.purgeOlderThan(cutoff);
    }
    catch (...) {
    }
    co_return;
  });
}

void CameraObjectNotifier::flushDigests()
{
  const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  purgeFallbackLog(nowMs / 1000);
  for (const auto cameraId : policy_.trackedCameras()) {
    const std::string summary = policy_.takeDigest(cameraId, nowMs);
    if (summary.empty())
      continue;
    Json::Value digest;
    digest["cameraId"] = cameraId;
    deliver({.json = digest, .title = "Camera activity digest", .body = summary});
  }
}

void CameraObjectNotifier::deliver(const DeliverInput& input)
{
  const Json::Value& json = input.json;
  const std::string& title = input.title;
  const std::string& body = input.body;
  if (!notificationClient_) {
    LOG_WARN << "Camera notifier: notification SDK not configured";
    return;
  }

  drogon::async_run([json, title, body, this]() -> drogon::Task<void> {
    try {
      const auto userIds = co_await userRepository_.findNotifiableIds();
      if (userIds.empty()) {
        LOG_WARN << "Camera notifier: no owner/guard users to notify";
        co_return;
      }

      argus::notification::v1::CreateNotificationsRequest request;
      for (const auto userId : userIds)
        request.add_user_ids(userId);
      request.set_type("camera");
      request.set_title(title);
      request.set_body(body);
      request.set_data(json_util::toString(json));

      const auto response =
          co_await BlockingTask<NotificationCreateResult>([this, request]() {
            return notificationClient_->createNotifications(
                request,
                {.userId = 0, .role = "system", .device = "argus-gateway"});
          });
      if (response.outcome != NotificationRpcOutcome::Success) {
        LOG_WARN << "Camera notifier: notification delivery failed ("
                 << title << "): " << response.status.error_message();
        co_return;
      }
      LOG_INFO << "Camera notifier: notification delivered (" << title << ")";
    }
    catch (const std::exception& e) {
      LOG_ERROR << "Camera notifier: delivery failed: " << e.what();
    }
    co_return;
  });
}

namespace camera_notifier
{
CameraNotificationPolicy::Config resolveConfig()
{
  CameraNotificationPolicy::Config config;
  config.budgetPerHour = ConfigService::getInt("notifications.budget_per_hour");
  if (config.budgetPerHour <= 0)
    config.budgetPerHour = 6;
  config.silentStartHour =
      ConfigService::getInt("notifications.silent_start");
  config.silentEndHour = ConfigService::getInt("notifications.silent_end");
  const int timeoutS =
      ConfigService::getInt("notifications.guard_heartbeat_timeout_s");
  config.guardTimeoutMs =
      (timeoutS > 0 ? static_cast<int64_t>(timeoutS) : 30) * 1000;
  if (ConfigService::hasKey("notifications.fallback_min_score_median"))
    config.fallbackMinScoreMedian =
        ConfigService::getDouble("notifications.fallback_min_score_median");
  if (ConfigService::hasKey("notifications.fallback_min_dwell_ms"))
    config.fallbackMinDwellMs =
        ConfigService::getInt("notifications.fallback_min_dwell_ms");
  if (ConfigService::hasKey("notifications.fallback_suppress_known"))
    config.fallbackSuppressKnown =
        ConfigService::getBool("notifications.fallback_suppress_known");
  if (ConfigService::hasKey("notifications.fallback_retention_days"))
    config.fallbackRetentionDays =
        ConfigService::getInt("notifications.fallback_retention_days");
  return config;
}

CameraNotificationPolicy* subscribeObjectDetected(
    NatsBus& bus, std::shared_ptr<NotificationClient> client)
{
  static CameraObjectNotifier notifier(resolveConfig(), std::move(client));
  bus.subscribe(nats_subject::kGuardHeartbeat,
                [](std::string_view, std::string_view payload) {
                  const Json::Value heartbeat =
                      json_util::fromString(std::string(payload));
                  if (!heartbeat.isObject() ||
                      heartbeat.get("service", "").asString() !=
                          "argus-guard" ||
                      !heartbeat.get("enabled", false).asBool())
                    return;
                  drogon::app().getIOLoop(0)->runInLoop([]() {
                    const auto nowMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
                    notifier.policy().markGuardHeartbeat(nowMs);
                  });
                });
  bus.subscribe(nats_subject::kCameraObjectDetected,
                [](std::string_view, std::string_view payload) {
                  // cnats dispatcher thread: marshal into the Drogon loop.
                  drogon::app().getIOLoop(0)->runInLoop(
                      [payload = std::string(payload)]() {
                        notifier.handle(json_util::fromString(payload));
                      });
                });
  drogon::app().getLoop()->runEvery(
      std::chrono::minutes(1), []() { notifier.flushDigests(); });
  return &notifier.policy();
}
} // namespace camera_notifier
