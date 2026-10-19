#pragma once

#include <feature/voice/voice-engine-seam.hxx>
#include <feature/watch/camera-ear.hxx>
#include <feature/watch/camera-gains.hxx>
#include <feature/watch/room-directory.hxx>
#include <feature/watch/echo-guard.hxx>
#include <feature/watch/fire-log.hxx>
#include <feature/watch/stage-two.hxx>
#include <feature/watch/wake-spotter.hxx>

#include <camera/camera-watch-client.hxx>
#include <guard/guard-presence-client.hxx>
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
  std::shared_ptr<const GuardPresenceClient> guard;
  std::shared_ptr<RoomDirectory> rooms;
  std::function<void(const ConfirmedWake&)> onWake;
  std::function<bool(const EarFire&)> onPlaybackFire;
  std::function<void(const EarFrame&)> onFrame;
};

struct WakeDecisionInput
{
  const std::vector<EarFire>& fires;
  const std::map<int64_t, WatchCameraInfo>& cameras;
  const VoiceWatchConfig& config;
  std::function<float(int64_t, float)> normalize;
};

struct WakeCandidate
{
  size_t fire{0};
  float threshold{0.0F};
  float normalizedSnr{0.0F};
};

struct WakeScoreInput
{
  float wake{1.0F};
  float normalizedSnr{0.0F};
  float stageTwo{0.0F};
};

[[nodiscard]] float wakeScore(const WakeScoreInput& input, const VoiceWatchConfig& config);

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
  [[nodiscard]] bool listening(int64_t cameraId) const;
  [[nodiscard]] const std::shared_ptr<RoomDirectory>& rooms() const { return input_.rooms; }
  [[nodiscard]] CameraGains& gains() { return gains_; }

private:
  void refreshLoop(const std::stop_token& stop);
  void refresh();
  void workerLoop(const std::stop_token& stop);
  void enqueue(EarFire fire);
  void decide(std::vector<EarFire> fires);
  struct Evaluation
  {
    WakeVerdict verdict{WakeVerdict::Unconfirmed};
    StageTwoResult stageTwo;
    float score{0.0F};
  };

  [[nodiscard]] Evaluation evaluate(const EarFire& fire, const WakeCandidate& candidate);
  void record(const EarFire& fire, WakeVerdict verdict, const Evaluation* evaluation);
  [[nodiscard]] bool sameEnvironment(int64_t left, int64_t right) const;

  WatchCoordinatorInput input_;
  StageTwo stageTwo_;
  FireLog fireLog_;
  CameraGains gains_;
  mutable std::mutex mutex_;
  std::map<int64_t, WatchCameraInfo> cameras_;
  std::map<int64_t, std::unique_ptr<CameraEar>> ears_;
  std::mutex queueMutex_;
  std::condition_variable_any queueReady_;
  std::deque<EarFire> queue_;
  std::jthread worker_;
  std::jthread refresher_;
};
