#include <feature/camera-notification/services/camera-notification-policy.hxx>

#include <ctime>
#include <json/value.h>

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
}

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
