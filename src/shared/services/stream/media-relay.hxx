#pragma once

#include <atomic>
#include <cstdint>
#include <drogon/HttpResponse.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

enum class MediaFormat
{
  Fmp4,
  MpegTs,
  Mjpeg
};

const char* toString(MediaFormat format);
MediaFormat mediaFormatFromString(const std::string& name);

struct MediaRelayStats
{
  int activeViewers{0};
  int64_t bytesRelayed{0};
  int rejected{0};
};

// Relays already-encoded media from go2rtc over the backend's own HTTPS port, no re-encoding.
class MediaRelay
{
public:
  MediaRelay() = default;
  ~MediaRelay();

  MediaRelay(const MediaRelay&) = delete;
  MediaRelay& operator=(const MediaRelay&) = delete;

  static MediaRelay& instance();

  void init();
  void shutdown();

  drogon::HttpResponsePtr stream(int64_t cameraId, MediaFormat format);
  drogon::HttpResponsePtr snapshot(int64_t cameraId);

  // Raw JPEG bytes of the last frame go2rtc has for the camera; no new session opened.
  std::string snapshotBytes(int64_t cameraId);

  MediaRelayStats stats();

private:
  static std::string upstreamPath(int64_t cameraId, MediaFormat format);
  bool acquireSlot(int64_t cameraId);
  void releaseSlot(int64_t cameraId);

  std::mutex mutex_;
  std::unordered_map<int64_t, int> viewers_;
  std::atomic<int64_t> bytesRelayed_{0};
  std::atomic<int> rejected_{0};
  int maxViewersPerCamera_ = 4;
  int maxTotalViewers_ = 8;
  size_t chunkSize_ = 32768;
};
