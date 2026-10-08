#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <config/camera-config.hxx>
#include <drogon/utils/coroutine.h>
#include <feature/monitor/camera-presence.hxx>
#include <feature/monitor/health-event.hxx>
#include <shared/utils/in-flight/in-flight.hxx>

class IFrameSource;

class CameraHealthMonitor
{
public:
  struct Dependencies
  {
    IFrameSource* source{nullptr};
    IHealthEventSink* sink{nullptr};
    ICameraPresenceSink* presence{nullptr};
    std::function<bool()> active{};
  };

  struct CameraRef
  {
    int64_t id{0};
    std::string name;
  };

  CameraHealthMonitor(Dependencies dependencies, CameraHealthConfig config);
  ~CameraHealthMonitor();

  void start();
  void requestStop();

  [[nodiscard]] bool drained() const;

  [[nodiscard]] bool running() const { return running_.load(); }

  [[nodiscard]] bool idle() const { return idle_.load(); }

  drogon::Task<void> tick(CameraRef camera);

private:
  struct CameraState
  {
    std::vector<uint8_t> reference;
    int64_t referenceAtMs{0};
    std::vector<uint8_t> previous;
    int64_t newSceneSinceMs{0};
    CameraHealthState lastStatus{CameraHealthState::Ok};
    int64_t lastPublishMs{0};
    bool published{false};
    int64_t sampledAtMs{0};
    bool sampled{false};
    int misses{0};
  };

  struct TickInput
  {
    CameraRef camera;
    std::vector<uint8_t> rgb;
    int width{0};
    int height{0};
    int64_t capturedAtMs{0};
  };

  drogon::Task<void> run();

  std::vector<CameraRef> loadCameras();
  [[nodiscard]] bool dueForSample(int64_t cameraId);

  HealthMetrics measure(const TickInput& input, CameraState& state) const;

  static constexpr int kMissesBeforeOffline = CameraPresenceRecorder::kMissesBeforeOffline;

  Dependencies dependencies_;
  CameraHealthConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> idle_{false};
  std::atomic<int64_t> inFlight_{0};
  std::mutex stateMutex_;
  std::map<int64_t, CameraState> states_;
};
