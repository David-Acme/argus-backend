#pragma once

#include <feature/voice/voice-engine-seam.hxx>
#include <feature/watch/camera-level.hxx>
#include <feature/watch/echo-guard.hxx>
#include <feature/watch/wake-spotter.hxx>

#include <camera/camera-watch-client.hxx>
#include <config/voice-config.hxx>
#include <shared/services/vad/vad-service.hxx>
#include <shared/wrapper/audio/sample-ring.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

inline constexpr int kWatchRate = 16000;

struct WakeCapture
{
  std::mutex mutex;
  std::condition_variable done;
  std::vector<float> samples;
  size_t wanted{0};
  bool complete{false};
  WatchClock::time_point start{};

  void append(std::span<const float> frame);
  [[nodiscard]] bool wait(std::chrono::milliseconds timeout);
  [[nodiscard]] std::vector<float> snapshot();
};

struct EarFire
{
  int64_t cameraId{0};
  WatchClock::time_point at{};
  std::string_view source;
  float snrDb{0.0F};
  float floorDb{0.0F};
  bool duringPlayback{false};
  std::shared_ptr<WakeCapture> capture;
  std::vector<float> envelope;
  WatchClock::time_point envelopeStart{};
  std::optional<std::string> transcript;
};

struct EarFrame
{
  int64_t cameraId{0};
  std::span<const float> samples;
  bool speech{false};
  float db{-120.0F};
  float snrDb{0.0F};
  WatchClock::time_point at{};
  bool gap{false};
};

struct CameraEarInput
{
  int64_t cameraId{0};
  std::string name;
  std::shared_ptr<const CameraWatchClient> camera;
  IWakeSpotter* spotter{nullptr};
  IVoiceVad* vad{nullptr};
  IVoiceStt* stt{nullptr};
  std::string language;
  VoiceWatchConfig config;
  std::function<void(EarFire)> onFire;
  std::function<void(const EarFrame&)> onFrame;
  std::function<bool(int64_t)> playing;
};

class CameraEar
{
public:
  explicit CameraEar(CameraEarInput input);
  ~CameraEar();

  CameraEar(const CameraEar&) = delete;
  CameraEar& operator=(const CameraEar&) = delete;

  void start();
  void stop();

  [[nodiscard]] int64_t cameraId() const { return input_.cameraId; }
  [[nodiscard]] const std::string& name() const { return input_.name; }
  [[nodiscard]] float snrDb() const;
  [[nodiscard]] bool speaking() const { return speech_.load(); }
  [[nodiscard]] bool listening() const { return listening_.load(); }

private:
  void readLoop(const std::stop_token& stop);
  void onPcm(const argus::camera::v1::MicrophoneFrame& frame);
  void feedSpotter(std::span<const float> frame, bool speech, WatchClock::time_point now);
  void watchSttSegment(bool speech, WatchClock::time_point now);
  void transcribeSegment(std::vector<float> segment);
  void fire(std::string_view source, std::optional<std::string> transcript);
  [[nodiscard]] std::vector<float> ringTail(size_t samples);

  CameraEarInput input_;
  mutable std::mutex mutex_;
  SampleRing ring_;
  CameraLevel level_;
  VadService vad_;
  std::vector<std::shared_ptr<WakeCapture>> captures_;
  std::unique_ptr<IWakeStream> spot_;
  WatchClock::time_point spotRetryAt_{};
  WatchClock::time_point lastSpeechAt_{};
  WatchClock::time_point lastFireAt_{};
  WatchClock::time_point segmentStart_{};
  bool inSegment_{false};
  bool segmentSent_{false};
  bool kwsSeen_{false};
  std::atomic<bool> transcribing_{false};
  std::atomic<bool> speech_{false};
  std::atomic<bool> listening_{false};
  std::jthread segmentThread_;
  std::jthread reader_;
};
