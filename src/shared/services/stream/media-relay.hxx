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

// Relays already-encoded media from the internal go2rtc instance to the client
// over the backend's own HTTPS port. Nothing is decoded or re-encoded: bytes
// move from a loopback socket to a chunked response, so the cost is a memcpy
// per fragment rather than a codec.
//
// Going through the backend instead of handing the client a direct WebRTC peer
// is what makes remote access work: a tunnel only proxies this one port, and a
// peer-to-peer media path would need TURN and extra ports to survive it.
class MediaRelay
{
public:
  MediaRelay() = delete;
  ~MediaRelay() = delete;

  static void init();
  static void shutdown();

  static drogon::HttpResponsePtr stream(int64_t cameraId, MediaFormat format);
  static drogon::HttpResponsePtr snapshot(int64_t cameraId);

  static MediaRelayStats stats();

private:
  static std::string upstreamPath(int64_t cameraId, MediaFormat format);
  static bool acquireSlot(int64_t cameraId);
  static void releaseSlot(int64_t cameraId);

  static std::mutex mutex_;
  static std::unordered_map<int64_t, int> viewers_;
  static std::atomic<int64_t> bytesRelayed_;
  static std::atomic<int> rejected_;
  static int maxViewersPerCamera_;
  static int maxTotalViewers_;
  static size_t chunkSize_;
};
