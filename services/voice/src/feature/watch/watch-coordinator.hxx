#pragma once

#include <feature/voice/voice-engine-seam.hxx>
#include <feature/watch/camera-ear.hxx>
#include <feature/watch/echo-guard.hxx>
#include <feature/watch/fire-log.hxx>
#include <feature/watch/stage-two.hxx>
#include <feature/watch/wake-spotter.hxx>

#include <camera/camera-watch-client.hxx>
#include <config/voice-config.hxx>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct WatchCameraInfo
{
  int64_t id{0};
  std::string name;
  bool talk{false};
  bool panTilt{false};
  bool tvExclusion{false};
};

struct ConfirmedWake
{
  int64_t cameraId{0};
  std::string cameraName;
  StageTwoResult stageTwo;
  std::shared_ptr<WakeCapture> capture;
  float snrDb{0.0F};
  WatchClock::time_point at{};
};

struct WatchCoordinatorInput
{
  VoiceWatchConfig config;
  std::shared_ptr<const CameraWatchClient> camera;
  IWakeSpotter* spotter{nullptr};
  IVoiceStt* stt{nullptr};
  IVoiceVad* vad{nullptr};
  std::string language;
  std::shared_ptr<EchoGuard> echo;
  std::function<bool(int64_t, int64_t)> sameEnvironment;
  std::function<void(const ConfirmedWake&)> onWake;
  std::function<void(const EarFrame&)> onFrame;
};

struct WakeDecisionInput
{
  const std::vector<EarFire>& fires;
  const std::map<int64_t, WatchCameraInfo>& cameras;
  const VoiceWatchConfig& config;
};

struct WakeCandidate
{
  size_t fire{0};
  float threshold{0.0F};
};

struct WakeDecision
{
  bool broadcast{false};
  std::vector<WakeCandidate> candidates;
  std::vector<size_t> duplicates;
  std::vector<size_t> quiet;
};

[[nodiscard]] bool broadcastBurst(const WakeDecisionInput& input);

[[nodiscard]] WakeDecision decideWakes(const WakeDecisionInput& input);

class WatchCoordinator
{
public:
  explicit WatchCoordinator(WatchCoordinatorInput input);
  ~WatchCoordinator();

  WatchCoordinator(const WatchCoordinator&) = delete;
  WatchCoordinator& operator=(const WatchCoordinator&) = delete;

  void start();
  void shutdown();

  [[nodiscard]] std::optional<WatchCameraInfo> camera(int64_t cameraId) const;
  [[nodiscard]] std::vector<WatchCameraInfo> cameras() const;
  [[nodiscard]] const FireLog& fireLog() const { return fireLog_; }
  [[nodiscard]] float snrOf(int64_t cameraId) const;

private:
  void refreshLoop(const std::stop_token& stop);
  void refresh();
  void workerLoop(const std::stop_token& stop);
  void enqueue(EarFire fire);
  void decide(std::vector<EarFire> fires);
  void confirm(const EarFire& fire, float threshold);
  void record(const EarFire& fire, WakeVerdict verdict, const StageTwoResult* stageTwo);

  WatchCoordinatorInput input_;
  StageTwo stageTwo_;
  FireLog fireLog_;
  mutable std::mutex mutex_;
  std::map<int64_t, WatchCameraInfo> cameras_;
  std::map<int64_t, std::unique_ptr<CameraEar>> ears_;
  std::mutex queueMutex_;
  std::condition_variable_any queueReady_;
  std::deque<EarFire> queue_;
  std::jthread worker_;
  std::jthread refresher_;
};
