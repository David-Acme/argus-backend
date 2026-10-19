#pragma once

#include <feature/watch/infra/microphone-source.hxx>

#include <argus/camera/v1/watch.pb.h>
#include <config/camera-config.hxx>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

struct MicrophoneChunk
{
  std::span<const int16_t> pcm;
  uint64_t sequence{0};
  int64_t capturedAtMs{0};
  bool gap{false};
};

struct MicrophoneSubscriber
{
  int64_t cameraId{0};
  std::function<void(const MicrophoneChunk&)> onFrame;
  std::function<void(argus::camera::v1::WatchRefusal)> onClosed;
};

struct MicrophoneSubscribed
{
  uint64_t id{0};
  argus::camera::v1::WatchRefusal refusal{argus::camera::v1::WATCH_REFUSAL_NONE};
};

struct MicrophoneHubInput
{
  MicrophoneSourceFactory sources;
  std::function<argus::camera::v1::WatchRefusal(int64_t)> gate;
  CameraWatchConfig config;
};

class MicrophoneHub
{
public:
  explicit MicrophoneHub(MicrophoneHubInput input);
  ~MicrophoneHub();

  MicrophoneHub(const MicrophoneHub&) = delete;
  MicrophoneHub& operator=(const MicrophoneHub&) = delete;

  [[nodiscard]] MicrophoneSubscribed subscribe(const MicrophoneSubscriber& subscriber);
  void unsubscribe(uint64_t id);
  void shutdown();

  [[nodiscard]] size_t readers() const;
  [[nodiscard]] size_t subscribers() const;

  [[nodiscard]] static size_t frameSamples(int64_t frameMs);

private:
  class Reader;

  void run(const std::shared_ptr<Reader>& reader, const std::stop_token& stop);
  void reap(std::vector<std::shared_ptr<Reader>>& retired);

  MicrophoneHubInput input_;
  mutable std::mutex mutex_;
  std::unordered_map<int64_t, std::shared_ptr<Reader>> readers_;
  std::unordered_map<uint64_t, int64_t> subscriptions_;
  std::vector<std::shared_ptr<Reader>> retired_;
  uint64_t nextId_{0};
  bool stopping_{false};
};
