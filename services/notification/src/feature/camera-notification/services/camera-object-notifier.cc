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
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace
{
FallbackNotice alertNotice(const Json::Value& event)
{
  return {.kind = FallbackNoticeKind::Alert,
          .cameraId = event.get("cameraId", 0).asInt64(),
          .cameraName = event.get("cameraName", "").asString(),
          .rule = event.get("rule", "").asString(),
          .suppressed = {}};
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

  const FallbackNotice notice = alertNotice(json);
  const std::string commandId = eventCommandId(json, atMs);
  deliver({.json = json, .notice = notice, .commandId = commandId});
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
    std::map<std::string, int> suppressed = policy_.takeDigest(cameraId, atMs);
    if (suppressed.empty())
      continue;
    Json::Value digest;
    digest["cameraId"] = cameraId;
    const FallbackNotice notice{.kind = FallbackNoticeKind::Digest,
                                .cameraId = cameraId,
                                .cameraName = {},
                                .rule = {},
                                .suppressed = std::move(suppressed)};
    const std::string commandId = digestCommandId(cameraId, atMs);
    deliver({.json = digest, .notice = notice, .commandId = commandId});
  }
}

void CameraObjectNotifier::deliver(const DeliverInput& input)
{
  const FallbackNotice& notice = input.notice;
  const std::string label =
      notice.cameraName.empty() ? "camera " + std::to_string(notice.cameraId)
                                : notice.cameraName;
  if (!identityClient_) {
    LOG_WARN << "Camera notifier: identity SDK not configured; notification "
                "skipped ("
             << label << ")";
    return;
  }

  drogon::async_run([this, json = input.json, notice, label,
                     commandId = input.commandId,
                     fallbackLang = policy_.config().lang]()
                        -> drogon::Task<void> {
    try {
      const auto batches = co_await BlockingTask<
          std::optional<std::map<std::string, std::vector<int64_t>>>>(
          [this, fallbackLang]()
              -> std::optional<std::map<std::string, std::vector<int64_t>>> {
            const auto userIds = identityClient_->listNotifiableUsers();
            if (!userIds)
              return std::nullopt;
            std::map<std::string, std::vector<int64_t>> byLang;
            for (const int64_t userId : *userIds) {
              const auto user = identityClient_->getUser(userId);
              byLang[camera_notification_copy::normalizeLang(
                         {.requested = user && user->has_user()
                                           ? user->user().lang()
                                           : std::string{},
                          .fallback = fallbackLang})]
                  .push_back(userId);
            }
            return byLang;
          });
      if (!batches) {
        LOG_WARN << "Camera notifier: identity roster unavailable; "
                    "notification skipped ("
                 << label << ")";
        co_return;
      }
      if (batches->empty()) {
        LOG_WARN << "Camera notifier: no owner/guard users to notify";
        co_return;
      }

      const bool digest = notice.kind == FallbackNoticeKind::Digest;
      for (const auto& [lang, userIds] : *batches) {
        const FallbackText text = camera_notification_copy::render(notice, lang);
        Json::Value data = json;
        data["kind"] = digest ? "camera_fallback_digest" : "camera_fallback";
        data["threadKey"] =
            "camera:fallback:" + std::to_string(notice.cameraId);
        data["urgency"] = digest ? "passive" : "time_sensitive";
        data["lang"] = lang;
        const NotificationCreateOutcome outcome =
            co_await notificationService_.createManyAndEmit(
                {.userIds = userIds,
                 .notification = {.userId = 0,
                                  .type = "camera",
                                  .title = text.title,
                                  .body = text.body,
                                  .data = data},
                 .commandId = batches->size() == 1 ? commandId
                                                   : commandId + ":" + lang});
        if (outcome.duplicate) {
          LOG_INFO << "Camera notifier: notification already recorded ("
                   << label << ")";
          continue;
        }
        LOG_INFO << "Camera notifier: notification created for "
                 << outcome.createdCount << " users (" << label << ")";
      }
    }
    catch (const std::exception& e) {
      LOG_WARN << "Camera notifier: delivery failed (" << label
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
  if (ConfigService::hasKey("notifications.silent_start"))
    config.silentStartHour =
        ConfigService::getInt("notifications.silent_start");
  if (ConfigService::hasKey("notifications.silent_end"))
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
  const std::string lang = ConfigService::getString("notifications.lang");
  config.lang =
      camera_notification_copy::normalizeLang({.requested = lang, .fallback = "es"});
  return config;
}

void refresh(CameraObjectNotifier& notifier)
{
  drogon::app().getIOLoop(0)->runInLoop(
      [&notifier, config = resolveConfig()]() {
        notifier.policy().reconfigure(config);
      });
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
