#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <shared/vocabulary/camera-stream-role.hxx>
#include <string>
#include <vector>

struct Go2rtcSource
{
  std::string name;
  std::string url;
  bool preload{false};
};

struct Go2rtcSourceChange
{
  std::vector<Go2rtcSource> upserts;
  std::vector<std::string> removals;
};

struct Go2rtcWebRtc
{
  std::string listen;
  std::vector<std::string> candidates;
};

struct Go2rtcConfigInput
{
  std::string api;
  std::string rtsp;
  Go2rtcWebRtc webrtc;
  const std::vector<Go2rtcSource>& sources;
};

struct Go2rtcStatus
{
  bool running{false};
  bool healthy{false};
  int restarts{0};
  int64_t pid{0};
  std::string lastError;
};

class Go2rtcManager
{
public:
  Go2rtcManager();
  ~Go2rtcManager();

  Go2rtcManager(const Go2rtcManager&) = delete;
  Go2rtcManager& operator=(const Go2rtcManager&) = delete;

  static Go2rtcManager& instance();

  void configure();
  void start();
  void init();
  void shutdown();

  bool isRunning();
  Go2rtcStatus status();

  bool applySources(const Go2rtcSourceChange& change);
  void requestRestart();
  [[nodiscard]] int64_t restartsServed() const;

  [[nodiscard]] bool webrtcEnabled() const;
  [[nodiscard]] std::vector<std::string> webrtcAnswerHosts();
  static std::vector<std::string> answerHostsOf(const std::vector<std::string>& candidates);

  std::string apiBase();
  std::string rtspBase();
  static std::string sourceName(int64_t cameraId, CameraStream stream);
  static std::string sourceFor(int64_t cameraId, CameraStreamRole role);
  static std::string opusAudioSource(const std::string& source);

  static bool isSafeName(const std::string& name);
  static bool isSafeUrl(const std::string& url);
  static bool isSafeListen(const std::string& listen);
  static bool isSafeCandidate(const std::string& candidate);
  static std::string renderConfig(const Go2rtcConfigInput& input);
  static std::string credentialVariable(const std::string& name);
  static std::string sealedUrl(const Go2rtcSource& source);
  static std::vector<std::string> credentialEnvironment(const std::vector<Go2rtcSource>& sources);

  bool healthCheck();
  bool waitReady(int maxMs);

private:
  bool writeConfig();
  bool spawn();
  void terminate();
  void supervise(const std::stop_token& stop);
  bool waitFor(const std::stop_token& stop, std::chrono::milliseconds limit);
  [[nodiscard]] bool restartDue();
  void serveRequestedRestart();
  void stopSupervisor();
  void setError(std::string error);
  bool merge(const Go2rtcSourceChange& change);
  bool respawn();

  std::vector<Go2rtcSource> sources_;
  std::mutex mutex_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> healthy_{false};
  std::atomic<int64_t> pid_{0};
  std::atomic<int> restarts_{0};
  std::mutex errorMutex_;
  std::string lastError_;
  std::string binPath_;
  std::string configPath_;
  std::string apiAddr_;
  std::string rtspAddr_;
  Go2rtcWebRtc webrtc_;
  std::atomic<bool> webrtcOn_{false};
  std::mutex hostsMutex_;
  std::vector<std::string> answerHosts_;
  std::mutex wakeMutex_;
  std::condition_variable_any wake_;
  std::optional<std::chrono::steady_clock::time_point> restartDueAt_;
  std::atomic<int64_t> restartsServed_{0};
  std::jthread supervisor_;

  int maxRestarts_ = 8;
};
