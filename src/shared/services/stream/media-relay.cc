#include "media-relay.hxx"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <drogon/drogon.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{

struct Upstream
{
  int fd{-1};
  std::string headers;
  std::string leftover;
  bool ok{false};
};

// Minimal blocking HTTP/1.1 GET against loopback. Drogon's HttpClient buffers
// the whole body, which is wrong for an endless media stream.
Upstream openUpstream(const std::string& host, int port,
                      const std::string& path, int timeoutSec)
{
  Upstream up;
  up.fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (up.fd < 0)
    return up;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(up.fd);
    up.fd = -1;
    return up;
  }

  timeval tv{};
  tv.tv_sec = timeoutSec;
  ::setsockopt(up.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(up.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  const int one = 1;
  ::setsockopt(up.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  if (::connect(up.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(up.fd);
    up.fd = -1;
    return up;
  }

  const std::string req = "GET " + path +
                          " HTTP/1.1\r\nHost: " + host +
                          "\r\nConnection: close\r\nUser-Agent: argus\r\n\r\n";
  if (::send(up.fd, req.data(), req.size(), 0) < 0) {
    ::close(up.fd);
    up.fd = -1;
    return up;
  }

  std::string buf;
  char tmp[4096];
  while (buf.find("\r\n\r\n") == std::string::npos) {
    const auto n = ::recv(up.fd, tmp, sizeof(tmp), 0);
    if (n <= 0) {
      ::close(up.fd);
      up.fd = -1;
      return up;
    }
    buf.append(tmp, static_cast<size_t>(n));
    if (buf.size() > 16384)
      break;
  }

  const auto sep = buf.find("\r\n\r\n");
  if (sep == std::string::npos) {
    ::close(up.fd);
    up.fd = -1;
    return up;
  }
  up.headers = buf.substr(0, sep);
  up.leftover = buf.substr(sep + 4);
  up.ok = up.headers.find(" 200") != std::string::npos;
  if (!up.ok) {
    ::close(up.fd);
    up.fd = -1;
  }
  return up;
}

std::pair<std::string, int> splitHostPort(const std::string& addr)
{
  const auto colon = addr.find(':');
  if (colon == std::string::npos)
    return {addr, 80};
  return {addr.substr(0, colon), std::atoi(addr.c_str() + colon + 1)};
}

} // namespace

const char* toString(MediaFormat format)
{
  switch (format) {
  case MediaFormat::MpegTs:
    return "mpegts";
  case MediaFormat::Mjpeg:
    return "mjpeg";
  case MediaFormat::Fmp4:
  default:
    return "fmp4";
  }
}

MediaFormat mediaFormatFromString(const std::string& name)
{
  if (name == "mpegts")
    return MediaFormat::MpegTs;
  if (name == "mjpeg")
    return MediaFormat::Mjpeg;
  return MediaFormat::Fmp4;
}

std::mutex MediaRelay::mutex_;
std::unordered_map<int64_t, int> MediaRelay::viewers_;
std::atomic<int64_t> MediaRelay::bytesRelayed_{0};
std::atomic<int> MediaRelay::rejected_{0};
int MediaRelay::maxViewersPerCamera_ = 4;
int MediaRelay::maxTotalViewers_ = 8;
size_t MediaRelay::chunkSize_ = 32768;

void MediaRelay::init()
{
  if (const int v = ConfigService::getInt("streaming.max_viewers_per_camera");
      v > 0)
    maxViewersPerCamera_ = v;
  if (const int v = ConfigService::getInt("streaming.max_total_viewers"); v > 0)
    maxTotalViewers_ = v;
  if (const int v = ConfigService::getInt("streaming.relay_chunk_bytes");
      v >= 4096)
    chunkSize_ = static_cast<size_t>(v);

  LOG_INFO << "MediaRelay ready (per_camera=" << maxViewersPerCamera_
           << ", total=" << maxTotalViewers_ << ", chunk=" << chunkSize_ << "B)";
}

void MediaRelay::shutdown()
{
  std::lock_guard<std::mutex> lock(mutex_);
  viewers_.clear();
  LOG_INFO << "MediaRelay shutdown";
}

MediaRelayStats MediaRelay::stats()
{
  MediaRelayStats s;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, n] : viewers_)
      s.activeViewers += n;
  }
  s.bytesRelayed = bytesRelayed_.load(std::memory_order_relaxed);
  s.rejected = rejected_.load(std::memory_order_relaxed);
  return s;
}

std::string MediaRelay::upstreamPath(int64_t cameraId, MediaFormat format)
{
  const std::string src = Go2rtcManager::streamName(cameraId);
  switch (format) {
  case MediaFormat::MpegTs:
    return "/api/stream.mpegts?src=" + src;
  case MediaFormat::Mjpeg:
    return "/api/stream.mjpeg?src=" + src;
  case MediaFormat::Fmp4:
  default:
    return "/api/stream.mp4?src=" + src;
  }
}

bool MediaRelay::acquireSlot(int64_t cameraId)
{
  std::lock_guard<std::mutex> lock(mutex_);
  int total = 0;
  for (const auto& [id, n] : viewers_)
    total += n;
  if (total >= maxTotalViewers_)
    return false;
  if (viewers_[cameraId] >= maxViewersPerCamera_)
    return false;
  ++viewers_[cameraId];
  return true;
}

void MediaRelay::releaseSlot(int64_t cameraId)
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = viewers_.find(cameraId);
  if (it == viewers_.end())
    return;
  if (--it->second <= 0)
    viewers_.erase(it);
}

drogon::HttpResponsePtr MediaRelay::stream(int64_t cameraId, MediaFormat format)
{
  if (!Go2rtcManager::isRunning()) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k503ServiceUnavailable);
    return resp;
  }

  if (!acquireSlot(cameraId)) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k429TooManyRequests);
    return resp;
  }

  const auto [host, port] = splitHostPort(Go2rtcManager::apiBase().substr(7));
  const std::string path = upstreamPath(cameraId, format);

  const char* contentType = "video/mp4";
  if (format == MediaFormat::MpegTs)
    contentType = "video/mp2t";
  else if (format == MediaFormat::Mjpeg)
    contentType = "multipart/x-mixed-replace; boundary=frame";

  auto resp = drogon::HttpResponse::newAsyncStreamResponse(
      [cameraId, host, port, path](drogon::ResponseStreamPtr stream) {
        auto shared =
            std::make_shared<drogon::ResponseStreamPtr>(std::move(stream));
        std::thread([cameraId, host, port, path, shared]() {
          Upstream up = openUpstream(host, port, path, 10);
          if (!up.ok) {
            LOG_WARN << "MediaRelay: upstream failed for cam" << cameraId;
            (*shared)->close();
            MediaRelay::releaseSlot(cameraId);
            return;
          }

          if (!up.leftover.empty()) {
            if (!(*shared)->send(up.leftover)) {
              ::close(up.fd);
              (*shared)->close();
              MediaRelay::releaseSlot(cameraId);
              return;
            }
            bytesRelayed_.fetch_add(
                static_cast<int64_t>(up.leftover.size()),
                std::memory_order_relaxed);
          }

          std::vector<char> buf(chunkSize_);
          for (;;) {
            const auto n = ::recv(up.fd, buf.data(), buf.size(), 0);
            if (n == 0)
              break;
            if (n < 0) {
              if (errno == EINTR)
                continue;
              break;
            }
            // send() returns false once the client is gone, which is how a
            // closed viewer tears the upstream down instead of leaking it.
            if (!(*shared)->send(std::string(buf.data(),
                                             static_cast<size_t>(n))))
              break;
            bytesRelayed_.fetch_add(static_cast<int64_t>(n),
                                    std::memory_order_relaxed);
          }

          ::close(up.fd);
          (*shared)->close();
          MediaRelay::releaseSlot(cameraId);
        }).detach();
      },
      true);

  resp->setContentTypeString(contentType);
  resp->addHeader("Cache-Control", "no-store");
  resp->addHeader("X-Accel-Buffering", "no");
  return resp;
}

drogon::HttpResponsePtr MediaRelay::snapshot(int64_t cameraId)
{
  if (!Go2rtcManager::isRunning()) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k503ServiceUnavailable);
    return resp;
  }

  const auto [host, port] = splitHostPort(Go2rtcManager::apiBase().substr(7));
  const std::string path =
      "/api/frame.jpeg?src=" + Go2rtcManager::streamName(cameraId);

  Upstream up = openUpstream(host, port, path, 5);
  if (!up.ok) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k502BadGateway);
    return resp;
  }

  std::string body = std::move(up.leftover);
  char tmp[16384];
  for (;;) {
    const auto n = ::recv(up.fd, tmp, sizeof(tmp), 0);
    if (n <= 0)
      break;
    body.append(tmp, static_cast<size_t>(n));
    if (body.size() > 8u * 1024u * 1024u)
      break;
  }
  ::close(up.fd);
  bytesRelayed_.fetch_add(static_cast<int64_t>(body.size()),
                          std::memory_order_relaxed);

  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setContentTypeString("image/jpeg");
  resp->setBody(std::move(body));
  resp->addHeader("Cache-Control", "no-store");
  return resp;
}
