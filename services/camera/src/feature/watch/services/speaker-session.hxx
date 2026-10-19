#pragma once

#include <feature/talk/talk-uplink.hxx>
#include <shared/services/camera-driver/camera-driver.hxx>

#include <argus/camera/v1/watch.pb.h>
#include <config/camera-config.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

struct SpeakerSessionInput
{
  int64_t cameraId{0};
  int sourceRate{16000};
  bool earcon{false};
  CameraWatchConfig config;
  std::shared_ptr<ICameraDriver> driver;
  std::function<argus::camera::v1::WatchRefusal()> gate;
  std::function<void(const argus::camera::v1::SpeakEvent&)> emit;
};

struct SpeakerEventInput
{
  argus::camera::v1::SpeakState state{argus::camera::v1::SPEAK_STATE_UNSPECIFIED};
  int64_t playedMs{0};
  argus::camera::v1::WatchRefusal refusal{argus::camera::v1::WATCH_REFUSAL_NONE};
  std::string detail;
};

class SpeakerSession
{
public:
  explicit SpeakerSession(SpeakerSessionInput input);
  ~SpeakerSession();

  SpeakerSession(const SpeakerSession&) = delete;
  SpeakerSession& operator=(const SpeakerSession&) = delete;

  void start();
  void push(std::span<const int16_t> samples);
  void finishInput();
  void stop();
  void join();

  [[nodiscard]] bool finished() const { return finished_.load(); }

private:
  using Clock = std::chrono::steady_clock;

  void run();
  void serve(ICameraTalkLine& line);
  void emit(const SpeakerEventInput& input) const;

  SpeakerSessionInput input_;
  TalkUplink uplink_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_{false};
  bool inputDone_{false};
  Clock::time_point lastInput_;
  std::atomic<bool> finished_{false};
  std::thread thread_;
};
