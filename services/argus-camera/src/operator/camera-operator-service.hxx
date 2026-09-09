#pragma once

#include <objects/object-detector.hxx>
#include <operator/event-intelligence.hxx>
#include <operator/frame-source.hxx>
#include <operator/object-event-sink.hxx>
#include <operator/operator-config.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct CameraRef
{
  int64_t id{0};
  std::string name;
};

// Per-camera operator loop on the Drogon event loop.
class CameraOperatorService
{
public:
  struct Dependencies
  {
    IObjectDetector* detector{nullptr};
    IFrameSource* source{nullptr};
    IObjectEventSink* sink{nullptr};
    const IKnownPersonMatcher* matcher{nullptr};
  };

  struct Inputs
  {
    Dependencies dependencies;
    ObjectsConfig objects;
    OperatorConfig operator_;
  };

  explicit CameraOperatorService(Inputs inputs);

  // Resolves the enabled cameras and launches one loop per camera.
  void start();

  void stop();

  bool running() const { return running_.load(); }

  struct ProcessFrameInput
  {
    int64_t cameraId{0};
    const std::string& cameraName;
    CameraFrame& frame;
  };

  // One synchronous pipeline step (decode -> detect -> rules -> aggregate -> publish).
  void processFrame(const ProcessFrameInput& input);

private:
  struct CameraState
  {
    std::optional<ObjectDetectedEvent> pending;
    int64_t pendingStartMs{0};
    std::map<std::string, int64_t> lastEmitByClass;
    int64_t vehicleLastSeenMs{0};
    int presenceStreak{0};
  };

  struct PublishPendingInput
  {
    int64_t cameraId{0};
    CameraState& state;
    int64_t nowMs{0};
  };

  drogon::Task<void> runCamera(CameraRef camera);

  bool isNightHour(int hour) const;

  void publishPending(const PublishPendingInput& input);

  Inputs inputs_;
  std::atomic<bool> running_{false};
  std::mutex stateMutex_;
  std::map<int64_t, CameraState> states_;
};
