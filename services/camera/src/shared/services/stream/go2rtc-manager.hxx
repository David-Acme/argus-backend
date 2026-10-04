#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
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

  void init();
  void shutdown();

  bool isRunning();
  Go2rtcStatus status();

  bool applySources(const Go2rtcSourceChange& change);

  std::string apiBase();
  std::string rtspBase();
  static std::string sourceName(int64_t cameraId, CameraStream stream);
  static std::string sourceFor(int64_t cameraId, CameraStreamRole role);

  static bool isSafeName(const std::string& name);
  static bool isSafeUrl(const std::string& url);

  bool healthCheck();
  bool waitReady(int maxMs);

private:
  bool writeConfig();
  bool spawn();
  void terminate();
  void supervise();
  void setError(std::string error);
  bool merge(const Go2rtcSourceChange& change);

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

  int maxRestarts_ = 8;
};
