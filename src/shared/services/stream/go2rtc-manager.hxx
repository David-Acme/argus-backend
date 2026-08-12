#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct Go2rtcSource
{
  std::string name;
  std::string url;
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

  bool addSource(const Go2rtcSource& source);
  bool removeSource(const std::string& name);

  std::string apiBase();
  static std::string streamName(int64_t cameraId);

  // Rejects anything that could break out of the generated YAML or of an
  // argument vector. Callers must validate before persisting a camera.
  static bool isSafeName(const std::string& name);
  static bool isSafeUrl(const std::string& url);

  bool healthCheck();
  bool waitReady(int maxMs);

private:
  bool writeConfig();
  bool spawn();
  void terminate();
  void supervise();

  std::vector<Go2rtcSource> sources_;
  std::mutex mutex_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> healthy_{false};
  int64_t pid_ = 0;
  int restarts_ = 0;
  std::string lastError_;
  std::string binPath_;
  std::string configPath_;
  std::string apiAddr_;
  std::string rtspAddr_;

  int maxRestarts_ = 8;
};
