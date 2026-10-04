#include "camera-live-board.hxx"

#include <algorithm>

Json::Value CameraLiveEvent::toJson() const
{
  Json::Value out(Json::objectValue);
  out["id"] = id;
  out["cameraId"] = static_cast<Json::Int64>(cameraId);
  out["at"] = static_cast<Json::Int64>(atMs);
  out["rule"] = rule;
  out["severity"] = severity;
  out["label"] = label;
  out["zoneName"] = zoneName;
  return out;
}

CameraLiveBoard& CameraLiveBoard::instance()
{
  static CameraLiveBoard board;
  return board;
}

void CameraLiveBoard::recordSample(const CameraLiveSample& sample)
{
  std::scoped_lock lock(mutex_);
  CameraLiveState& state = cameras_[sample.cameraId];
  state.sampledMs = sample.atMs;
  state.health = sample.health;
  if (sample.reachable) {
    state.lastSeenMs = sample.atMs;
    if (sample.width > 0 && sample.height > 0) {
      state.width = sample.width;
      state.height = sample.height;
    }
  }
}

void CameraLiveBoard::recordEvent(const CameraLiveEvent& event)
{
  std::scoped_lock lock(mutex_);
  if (std::ranges::any_of(events_, [&event](const auto& known) { return known.id == event.id; }))
    return;
  CameraLiveState& state = cameras_[event.cameraId];
  if (!state.lastEvent || state.lastEvent->atMs <= event.atMs)
    state.lastEvent = event;
  const auto at = std::ranges::upper_bound(events_, event.atMs, std::ranges::greater{}, &CameraLiveEvent::atMs);
  events_.insert(at, event);
  while (events_.size() > kRecentEvents)
    events_.pop_back();
}

void CameraLiveBoard::forget(int64_t cameraId)
{
  std::scoped_lock lock(mutex_);
  cameras_.erase(cameraId);
  std::erase_if(events_, [cameraId](const auto& event) { return event.cameraId == cameraId; });
}

std::unordered_map<int64_t, CameraLiveState> CameraLiveBoard::snapshot() const
{
  std::scoped_lock lock(mutex_);
  return cameras_;
}

std::vector<CameraLiveEvent> CameraLiveBoard::recentEvents(size_t limit) const
{
  std::scoped_lock lock(mutex_);
  const size_t count = std::min(limit, events_.size());
  return {events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(count)};
}

std::optional<CameraLiveEvent> camera_live_event::fromPayload(const Json::Value& payload)
{
  if (!payload.isObject() || !payload["eventId"].isString() || !payload["cameraId"].isIntegral())
    return std::nullopt;
  CameraLiveEvent event;
  event.id = payload["eventId"].asString();
  event.cameraId = payload["cameraId"].asInt64();
  event.atMs = payload.get("publishedAt", static_cast<Json::Int64>(0)).asInt64();
  event.rule = payload.get("rule", "").asString();
  event.severity = payload.get("severity", "").asString();
  const Json::Value& objects = payload["objects"];
  if (objects.isArray() && !objects.empty()) {
    event.label = objects[0].get("class", "").asString();
    for (const auto& object : objects) {
      if (object.isObject() && object["zoneName"].isString()) {
        event.zoneName = object["zoneName"].asString();
        break;
      }
    }
  }
  return event;
}
