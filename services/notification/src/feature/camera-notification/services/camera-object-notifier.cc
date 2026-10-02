#include <feature/camera-notification/services/camera-object-notifier.hxx>

#include <chrono>
#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/blocking-task.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <ctime>
#include <json/value.h>
#include <optional>
#include <utility>
#include <vector>

namespace
{
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

bool isHardSignal(const Json::Value& event)
{
  if (event.get("severity", "").asString() == "critical")
    return true;
  const std::string rule = event.get("rule", "").asString();
  return rule == "person_in_alert_zone";
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

std::string eventCommandId(const Json::Value& event, int64_t nowMs)
{
  const std::string eventId = event.get("eventId", "").asString();
  if (!eventId.empty())
    return "camera:" + eventId;
  return "camera:" + std::to_string(event.get("cameraId", 0).asInt64()) +
         ":event:" + std::to_string(nowMs);
}

std::string digestCommandId(int64_t cameraId, int64_t nowMs)
{
  return "camera-digest:" + std::to_string(cameraId) + ":" +
         std::to_string(nowMs);
}

int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

CameraObjectNotifier::CameraObjectNotifier(
    CameraNotificationPolicy::Config config,
    CameraNotifierDependencies dependencies)
    : notificationService_(std::move(dependencies.delivery)),
      identityClient_(std::move(dependencies.identityClient)),
      policy_(config)
{
}

void CameraObjectNotifier::handle(const Json::Value& json)
{
  if (!json.isObject()) {
    LOG_WARN << "Camera notifier: dropped malformed object_detected event";
    return;
  }

  const auto cameraId = json.get("cameraId", 0).asInt64();
  const int64_t atMs = nowMs();

  if (policy_.guardReady(atMs)) {
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
                 .createdAt = atMs / 1000});
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
                 .createdAt = atMs / 1000});
    return;
  }
  policy_.countFallbackPass();

  if (!policy_.shouldNotify(cameraId, atMs)) {
    for (const auto& object : json.get("objects", Json::Value()))
      policy_.countSuppressed(cameraId, object.get("class", "").asString());
    LOG_INFO << "Camera notifier: budget or silent hours suppressed camera "
             << cameraId;
    logFallback({.cameraId = cameraId,
                 .rule = json.get("rule", "").asString(),
                 .severity = json.get("severity", "").asString(),
                 .reason = FallbackDropReason::BudgetSilent,
                 .createdAt = atMs / 1000});
    return;
  }

  deliver({.json = json,
           .title = titleFor(json),
           .body = bodyFor(json),
           .commandId = eventCommandId(json, atMs)});
}

void CameraObjectNotifier::logFallback(const CameraFallbackLogInput& input)
{
  if (!DbService::client())
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
  const int64_t atMs = nowMs();
  purgeFallbackLog(atMs / 1000);
  for (const auto cameraId : policy_.trackedCameras()) {
    const std::string summary = policy_.takeDigest(cameraId, atMs);
    if (summary.empty())
      continue;
    Json::Value digest;
    digest["cameraId"] = cameraId;
    deliver({.json = digest,
             .title = "Camera activity digest",
             .body = summary,
             .commandId = digestCommandId(cameraId, atMs)});
  }
}

void CameraObjectNotifier::deliver(const DeliverInput& input)
{
  const Json::Value& json = input.json;
  const std::string& title = input.title;
  const std::string& body = input.body;
  const std::string& commandId = input.commandId;
  if (!identityClient_) {
    LOG_WARN << "Camera notifier: identity SDK not configured; notification "
                "skipped ("
             << title << ")";
    return;
  }

  drogon::async_run([this, json, title, body, commandId]()
                        -> drogon::Task<void> {
    try {
      const auto userIds =
          co_await BlockingTask<std::optional<std::vector<int64_t>>>(
              [this]() { return identityClient_->listNotifiableUsers(); });
      if (!userIds) {
        LOG_WARN << "Camera notifier: identity roster unavailable; "
                    "notification skipped ("
                 << title << ")";
        co_return;
      }
      if (userIds->empty()) {
        LOG_WARN << "Camera notifier: no owner/guard users to notify";
        co_return;
      }

      const NotificationCreateOutcome outcome =
          co_await notificationService_.createManyAndEmit(
              {.userIds = *userIds,
               .notification = {.userId = 0,
                                .type = "camera",
                                .title = title,
                                .body = body,
                                .data = json},
               .commandId = commandId});
      if (outcome.duplicate) {
        LOG_INFO << "Camera notifier: notification already recorded ("
                 << title << ")";
        co_return;
      }
      LOG_INFO << "Camera notifier: notification created for "
               << outcome.createdCount << " users (" << title << ")";
    }
    catch (const std::exception& e) {
      LOG_WARN << "Camera notifier: delivery failed (" << title
               << "): " << e.what();
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

void subscribe(NatsBus& bus, CameraObjectNotifier& notifier)
{
  bus.subscribe(nats_subject::kGuardHeartbeat,
                [&notifier](std::string_view, std::string_view payload) {
                  const Json::Value heartbeat =
                      json_util::fromString(std::string(payload));
                  if (!heartbeat.isObject() ||
                      heartbeat.get("service", "").asString() !=
                          "argus-guard" ||
                      !heartbeat.get("enabled", false).asBool())
                    return;
                  drogon::app().getIOLoop(0)->runInLoop([&notifier]() {
                    notifier.policy().markGuardHeartbeat(nowMs());
                  });
                });
  bus.subscribe(nats_subject::kCameraObjectDetected,
                [&notifier](std::string_view, std::string_view payload) {
                  drogon::app().getIOLoop(0)->runInLoop(
                      [&notifier, payload = std::string(payload)]() {
                        notifier.handle(json_util::fromString(payload));
                      });
                });
  drogon::app().getLoop()->runEvery(std::chrono::minutes(1), [&notifier]() {
    drogon::app().getIOLoop(0)->runInLoop(
        [&notifier]() { notifier.flushDigests(); });
  });
}
}
