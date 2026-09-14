#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <drogon/utils/coroutine.h>
#include <monitor/health-event.hxx>

class IFrameSource;

struct CameraHealthConfig
{
  bool enabled{true};
  int64_t intervalMs{60000};
  HealthThresholds thresholds;
};

// Periodic image-health checks: occlusion, blur and moved-camera detection.
class CameraHealthMonitor
{
public:
  struct Dependencies
  {
    IFrameSource* source{nullptr};
    IHealthEventSink* sink{nullptr};
  };

  struct CameraRef
  {
    int64_t id{0};
    std::string name;
  };

  CameraHealthMonitor(Dependencies dependencies, CameraHealthConfig config);
  ~CameraHealthMonitor();

  void start();
  void stop();

private:
  struct CameraState
  {
    std::vector<uint8_t> reference;
    CameraHealthState lastStatus{CameraHealthState::Ok};
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

  std::vector<CameraRef> loadCameras() const;

  HealthMetrics measure(const TickInput& input, CameraState& state) const;

  Dependencies dependencies_;
  CameraHealthConfig config_;
  std::atomic<bool> running_{false};
  std::mutex stateMutex_;
  std::map<int64_t, CameraState> states_;
};
