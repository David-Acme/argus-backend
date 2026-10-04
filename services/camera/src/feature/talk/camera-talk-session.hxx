#pragma once

#include "talk-uplink.hxx"

#include <shared/services/camera-driver/camera-driver.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

struct TalkSessionConfig
{
  int64_t cameraId{0};
  int sampleRate{16000};
  int packetMs{120};
  int idleMs{20000};
  int maxMs{15 * 60 * 1000};
  int maxQueuedMs{1000};
};

struct TalkSessionEvents
{
  std::function<void(const Json::Value&)> send;
};

class CameraTalkSession
{
public:
  CameraTalkSession(TalkSessionConfig config,
                    std::shared_ptr<ICameraDriver> driver,
                    TalkSessionEvents events);
  ~CameraTalkSession();

  CameraTalkSession(const CameraTalkSession&) = delete;
  CameraTalkSession& operator=(const CameraTalkSession&) = delete;

  void start();
  void push(std::span<const int16_t> samples);
  void stop(const std::string& reason);
  void join();

  [[nodiscard]] bool finished() const { return finished_.load(); }
  [[nodiscard]] int64_t cameraId() const { return config_.cameraId; }

private:
  using Clock = std::chrono::steady_clock;

  void run();
  void serve(ICameraTalkLine& line);
  void refuse(int status, const std::string& error) const;
  void closed(const std::string& reason) const;

  TalkSessionConfig config_;
  std::shared_ptr<ICameraDriver> driver_;
  TalkSessionEvents events_;
  TalkUplink uplink_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_{false};
  std::string reason_;
  Clock::time_point lastAudio_;
  std::atomic<bool> finished_{false};
  std::thread thread_;
};
