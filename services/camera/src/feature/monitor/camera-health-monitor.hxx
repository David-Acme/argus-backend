#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <config/camera-config.hxx>
#include <drogon/utils/coroutine.h>
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

  drogon::Task<void> tick(CameraRef camera);

private:
  struct CameraState
  {
    std::vector<uint8_t> reference;
    CameraHealthState lastStatus{CameraHealthState::Ok};
    int64_t lastPublishMs{0};
    bool published{false};
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

  HealthMetrics measure(const TickInput& input, CameraState& state) const;

  Dependencies dependencies_;
  CameraHealthConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<int64_t> inFlight_{0};
  std::mutex stateMutex_;
  std::map<int64_t, CameraState> states_;
};
