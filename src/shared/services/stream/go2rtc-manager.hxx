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
  Go2rtcManager() = delete;
  ~Go2rtcManager() = delete;

  static void init();
  static void shutdown();

  static bool isRunning();
  static Go2rtcStatus status();

  static bool addSource(const Go2rtcSource& source);
  static bool removeSource(const std::string& name);

  static std::string apiBase();
  static std::string streamName(int64_t cameraId);

  // Rejects anything that could break out of the generated YAML or of an
  // argument vector. Callers must validate before persisting a camera.
  static bool isSafeName(const std::string& name);
  static bool isSafeUrl(const std::string& url);

  static bool healthCheck();

private:
  static bool writeConfig();
  static bool spawn();
  static void terminate();
  static void supervise();

  static std::vector<Go2rtcSource> sources_;
  static std::mutex mutex_;
  static std::atomic<bool> stopping_;
  static std::atomic<bool> healthy_;
  static int64_t pid_;
  static int restarts_;
  static std::string lastError_;
  static std::string binPath_;
  static std::string configPath_;
  static std::string apiAddr_;
  static std::string rtspAddr_;
  static int maxRestarts_;
};
