#include <sync/camera-notifier.hxx>

#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
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

bool CameraNotificationPolicy::shouldNotify(int64_t cameraId, int64_t nowMs)
{
  auto& state = windows_[cameraId];
  if (state.windowStartMs == 0 || nowMs - state.windowStartMs >= kHourMs) {
    state.windowStartMs = nowMs;
    state.notified = 0;
    state.suppressedByClass.clear();
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
  const bool silentOver =
      !inSilentHours(config_, hourOfDay(nowMs)) &&
      inSilentHours(config_, hourOfDay(nowMs - 1));
  const bool hourRolled =
      state.windowStartMs != 0 && nowMs - state.windowStartMs >= kHourMs;
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
  return std::to_string(total) + " events suppressed (" + summary + ")";
}

CameraObjectNotifier::CameraObjectNotifier(
    CameraNotificationPolicy::Config config)
    : policy_(config)
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

  if (!policy_.shouldNotify(cameraId, nowMs)) {
    for (const auto& object : json.get("objects", Json::Value()))
      policy_.countSuppressed(cameraId, object.get("class", "").asString());
    LOG_INFO << "Camera notifier: budget or silent hours suppressed camera "
             << cameraId;
    return;
  }

  deliver(json, titleFor(json), bodyFor(json));
}

void CameraObjectNotifier::flushDigests()
{
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  for (const auto cameraId : policy_.trackedCameras()) {
    const std::string summary = policy_.takeDigest(cameraId, nowMs);
    if (summary.empty())
      continue;
    Json::Value digest;
    digest["cameraId"] = cameraId;
    deliver(digest, "Camera activity digest", summary);
  }
}

void CameraObjectNotifier::deliver(const Json::Value& json,
                                   const std::string& title,
                                   const std::string& body)
{
  drogon::async_run([json, title, body, this]() -> drogon::Task<void> {
    try {
      auto rows = co_await DbService::client()->execSqlCoro(
          "SELECT id FROM user WHERE deleted_at IS NULL AND is_active = 1 "
          "AND role IN ('owner', 'guard')");
      if (rows.empty()) {
        LOG_WARN << "Camera notifier: no owner/guard users to notify";
        co_return;
      }
      std::vector<int64_t> userIds;
      userIds.reserve(rows.size());
      for (const auto& row : rows)
        userIds.push_back(row["id"].as<int64_t>());

      NotificationCreateInput input;
      input.type = "camera";
      input.title = title;
      input.body = body;
      input.data = json;
      co_await notificationService_.createAndEmitMany(userIds, input);
      LOG_INFO << "Camera notifier: notification delivered ("
               << title << ")";
    }
    catch (const std::exception& e) {
      LOG_ERROR << "Camera notifier: delivery failed: " << e.what();
    }
    co_return;
  });
}

namespace camera_notifier
{
namespace
{
CameraObjectNotifier& notifier()
{
  static CameraObjectNotifier instance(resolveConfig());
  return instance;
}
} // namespace

CameraNotificationPolicy::Config resolveConfig()
{
  CameraNotificationPolicy::Config config;
  config.budgetPerHour = ConfigService::getInt("notifications.budget_per_hour");
  if (config.budgetPerHour <= 0)
    config.budgetPerHour = 6;
  config.silentStartHour =
      ConfigService::getInt("notifications.silent_start");
  config.silentEndHour = ConfigService::getInt("notifications.silent_end");
  return config;
}

void subscribeObjectDetected(NatsBus& bus)
{
  bus.subscribe(nats_subject::kCameraObjectDetected,
                [](std::string_view, std::string_view payload) {
                  // cnats dispatcher thread: marshal into the Drogon loop.
                  drogon::app().getIOLoop(0)->runInLoop(
                      [payload = std::string(payload)]() {
                        notifier().handle(json_util::fromString(payload));
                      });
                });
  drogon::app().getLoop()->runEvery(
      std::chrono::minutes(1), []() { notifier().flushDigests(); });
}
} // namespace camera_notifier