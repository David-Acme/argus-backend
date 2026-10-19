#pragma once

#include <array>
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
#include <shared/schemas/camera/camera-schema.hxx>
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
    std::function<void(int64_t)> homeDrift{};
    std::function<bool()> protects{};
    std::function<void(TamperOnset)> tamperOnset{};
  };

  struct CameraRef
  {
    int64_t id{0};
    std::string name;
    CameraSchema camera;
  };

  CameraHealthMonitor(Dependencies dependencies, CameraHealthConfig config);
  ~CameraHealthMonitor();

  void start();
  void requestStop();

  [[nodiscard]] bool drained() const;

  [[nodiscard]] bool running() const { return running_.load(); }

  [[nodiscard]] bool idle() const { return idle_.load(); }

  drogon::Task<void> tick(CameraRef camera);

  void publishPrivacyState(const CameraRef& camera);

private:
  static constexpr int kGridSide = 4;
  using BlockSpread = std::array<double, static_cast<size_t>(kGridSide * kGridSide)>;

  struct Reference
  {
    std::vector<uint8_t> gray;
    int64_t atMs{0};
    BlockSpread blocks{};
    double laplacian{0.0};
    double sharpness{0.0};
  };

  struct CameraState
  {
    std::array<Reference, 2> references{};
    bool infraredKnown{false};
    bool infrared{false};
    int64_t holdUntilMs{0};
    bool previousKnown{false};
    double previousBrightness{0.0};
    double previousSharpness{1.0};
    int64_t suddenSinceMs{0};
    bool lastSudden{false};
    int64_t onsetAtMs{0};
    std::vector<uint8_t> lastGoodJpeg;
    int64_t lastGoodAtMs{0};
    std::vector<uint8_t> previous;
    int64_t newSceneSinceMs{0};
    CameraHealthState lastStatus{CameraHealthState::Ok};
    int64_t lastPublishMs{0};
    bool published{false};
    int64_t sampledAtMs{0};
    bool sampled{false};
    int misses{0};
    int64_t privacyBackoffUntilMs{0};
    int64_t homeCheckedForMs{0};
  };

  struct TickInput
  {
    CameraRef camera;
    std::vector<uint8_t> rgb;
    int width{0};
    int height{0};
    int64_t capturedAtMs{0};
    double chroma{0.0};
    int64_t intervalMs{0};
  };

  drogon::Task<void> run();

  std::vector<CameraRef> loadCameras();
  drogon::Task<bool> refreshPrivacy(const CameraRef& camera);
  [[nodiscard]] bool dueForSample(int64_t cameraId);
  [[nodiscard]] int64_t intervalNow() const;

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
