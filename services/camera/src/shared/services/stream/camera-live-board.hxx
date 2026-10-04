#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <json/value.h>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct CameraLiveSample
{
  int64_t cameraId{0};
  bool reachable{false};
  std::string health;
  int64_t atMs{0};
  int width{0};
  int height{0};
};

struct CameraLiveEvent
{
  std::string id;
  int64_t cameraId{0};
  int64_t atMs{0};
  std::string rule;
  std::string severity;
  std::string label;
  std::string zoneName;

  [[nodiscard]] Json::Value toJson() const;
};

struct CameraLiveState
{
  int64_t lastSeenMs{0};
  int64_t sampledMs{0};
  std::string health;
  int width{0};
  int height{0};
  std::optional<CameraLiveEvent> lastEvent;
};

class CameraLiveBoard
{
public:
  static constexpr size_t kRecentEvents = 60;

  static CameraLiveBoard& instance();

  void recordSample(const CameraLiveSample& sample);
  void recordEvent(const CameraLiveEvent& event);
  void forget(int64_t cameraId);

  [[nodiscard]] std::unordered_map<int64_t, CameraLiveState> snapshot() const;
  [[nodiscard]] std::vector<CameraLiveEvent> recentEvents(size_t limit) const;

private:
  mutable std::mutex mutex_;
  std::unordered_map<int64_t, CameraLiveState> cameras_;
  std::deque<CameraLiveEvent> events_;
};

namespace camera_live_event
{
std::optional<CameraLiveEvent> fromPayload(const Json::Value& payload);
}
