#include "stream-hub.hxx"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/upstream-http.hxx>
#include <shared/services/stream/ws-frame.hxx>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
constexpr size_t kDefaultChunkBytes = 16 * 1024;
constexpr int64_t kDefaultGraceMs = 2000;

std::string upstreamName(int64_t cameraId, const std::string& quality)
{
  const std::string base = Go2rtcManager::streamName(cameraId);
  return quality == "sub" ? base + "-sub" : base;
}
} // namespace

StreamHub::~StreamHub()
{
  shutdown();
}

StreamHub& StreamHub::instance()
{
  static StreamHub hub;
  return hub;
}

void StreamHub::sendFramed(const SendFramedInput& input)
{
  const std::shared_ptr<Subscriber>& sub = input.sub;
  ws_frame::Header h;
  h.type = input.type;
  h.keyframe = input.keyframe;
  h.subId = sub->subId;
  h.seq = ++sub->seq;
  const std::string blob =
      ws_frame::frame({.header = h, .payload = input.data, .len = input.len});
  if (!sub->sink->sendBinary(reinterpret_cast<const uint8_t*>(blob.data()),
                             blob.size())) {
    sub->sink = nullptr;
  }
}

void StreamHub::sendBox(const SendBoxInput& input)
{
  const std::shared_ptr<Subscriber>& sub = input.sub;
  const std::string& box = input.box;
  const bool keyframe = input.keyframe;
  size_t offset = 0;
  bool first = true;
  while (offset < box.size()) {
    const size_t part = std::min(chunkBytes_, box.size() - offset);
    if (!sub->sink->tryReserve(part)) {
      sub->skipUntilKeyframe = true;
      return;
    }
    sendFramed({.sub = sub,
                .type = ws_frame::kTypeMedia,
                .keyframe = keyframe && first,
                .data = reinterpret_cast<const uint8_t*>(box.data() + offset),
                .len = part});
    if (!sub->sink)
      return;
    offset += part;
    first = false;
  }
}

void StreamHub::dispatchBox(const DispatchBoxInput& input)
{
  Upstream& up = input.up;
  std::string box = std::move(input.box);
  const bool keyframe = input.keyframe;
  std::lock_guard<std::mutex> lock(up.mtx);
  for (auto& sub : up.subs) {
    if (!sub->sink)
      continue;
    if (sub->skipUntilKeyframe) {
      if (!keyframe)
        continue;
      sub->skipUntilKeyframe = false;
      if (!sub->sentInit && up.hasInit) {
        if (!sub->sink->tryReserve(up.init.size())) {
          sub->skipUntilKeyframe = true;
          continue;
        }
        sendFramed({.sub = sub,
                    .type = ws_frame::kTypeInit,
                    .keyframe = true,
                    .data = reinterpret_cast<const uint8_t*>(up.init.data()),
                    .len = up.init.size()});
        if (!sub->sink)
          continue;
        sub->sentInit = true;
      }
    }
    sendBox({.sub = sub, .box = box, .keyframe = keyframe});
  }

  up.subs.erase(std::remove_if(up.subs.begin(), up.subs.end(),
                               [](const auto& s) { return !s->sink; }),
                up.subs.end());
}

void StreamHub::runUpstream(std::shared_ptr<Upstream> up)
{
  const auto [host, port] = upstream_http::splitHostPort(
      Go2rtcManager::instance().apiBase().substr(7));
  const std::string path = "/api/stream.mp4?src=" + up->name;

  upstream_http::Upstream conn =
      upstream_http::open({.host = host, .port = port, .path = path,
                           .timeoutSec = 10});
  if (!conn.ok) {
    LOG_WARN << "StreamHub: upstream failed for " << up->name;
    std::lock_guard<std::mutex> lock(up->mtx);
    for (auto& sub : up->subs) {
      if (sub->sink)
        sub->sink->onClosed({.subId = sub->subId, .reason = "upstream_failed"});
    }
    up->subs.clear();
    up->dead.store(true, std::memory_order_release);
    return;
  }
  up->fd.store(conn.fd);

  timeval tv{};
  tv.tv_sec = 1;
  ::setsockopt(conn.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  upstream_http::Fmp4Reader reader(
      {.chunked = upstream_http::isChunked(conn.headers)});
  reader.onInit = [&up](std::string box) {
    std::lock_guard<std::mutex> lock(up->mtx);
    up->init = std::move(box);
    up->hasInit = true;
  };
  reader.onFragment = [this, &up](std::string box, bool keyframe) {
    dispatchBox({.up = *up, .box = std::move(box), .keyframe = keyframe});
  };
  if (!conn.leftover.empty())
    reader.feed(conn.leftover.data(), conn.leftover.size());

  int64_t emptySinceMs = 0;
  char buf[65536];
  while (!up->stopping.load(std::memory_order_relaxed)) {
    const ssize_t n = ::recv(conn.fd, buf, sizeof(buf), 0);
    if (n > 0) {
      reader.feed(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0)
      break;
    if (errno == EINTR)
      continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      LOG_WARN << "StreamHub: recv failed on " << up->name
               << " errno=" << errno;
      break;
    }

    bool empty = false;
    {
      std::lock_guard<std::mutex> lock(up->mtx);
      empty = up->subs.empty();
    }
    if (empty) {
      const auto now = std::chrono::steady_clock::now().time_since_epoch();
      const int64_t nowMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
      if (emptySinceMs == 0)
        emptySinceMs = nowMs;
      else if (nowMs - emptySinceMs >= graceMs_)
        break;
    }
    else {
      emptySinceMs = 0;
    }
  }

  ::close(conn.fd);
  up->fd.store(-1);
  std::lock_guard<std::mutex> lock(up->mtx);
  for (auto& sub : up->subs) {
    if (sub->sink)
      sub->sink->onClosed({.subId = sub->subId, .reason = "upstream_closed"});
  }
  up->subs.clear();
  up->dead.store(true, std::memory_order_release);
}

void StreamHub::init()
{
  if (const int v = ConfigService::getInt("streaming.hub_chunk_bytes");
      v >= 1024)
    chunkBytes_ = static_cast<size_t>(v);
  if (const int64_t v = ConfigService::getInt("streaming.hub_grace_ms"); v > 0)
    graceMs_ = v;
  LOG_INFO << "StreamHub ready (chunk=" << chunkBytes_ << "B grace=" << graceMs_
           << "ms)";
}

void StreamHub::shutdown()
{
  std::lock_guard<std::mutex> lock(hubMutex_);
  for (auto& [name, up] : upstreams_) {
    up->stopping.store(true, std::memory_order_relaxed);
    const int fd = up->fd.exchange(-1);
    if (fd >= 0)
      ::close(fd);
  }
  for (auto& [name, up] : upstreams_) {
    if (up->reader.joinable())
      up->reader.join();
  }
  upstreams_.clear();
  subToUpstream_.clear();
}

std::shared_ptr<StreamHub::Upstream>
StreamHub::getOrOpen(const SubscribeInput& input, std::string& error)
{
  if (!Go2rtcManager::instance().isRunning()) {
    error = "go2rtc_not_running";
    return nullptr;
  }

  const std::string name = upstreamName(input.cameraId, input.quality);
  std::lock_guard<std::mutex> lock(hubMutex_);
  auto it = upstreams_.find(name);
  if (it != upstreams_.end()) {
    if (!it->second->dead.load(std::memory_order_acquire))
      return it->second;
    if (it->second->reader.joinable())
      it->second->reader.join();
    upstreams_.erase(it);
  }

  auto up = std::make_shared<Upstream>();
  up->name = name;
  up->reader = std::thread(&StreamHub::runUpstream, this, up);
  upstreams_.emplace(name, up);
  return up;
}

uint16_t StreamHub::subscribe(const SubscribeInput& input, std::string& error)
{
  if (!input.sink) {
    error = "invalid_sink";
    return 0;
  }

  auto up = getOrOpen(input, error);
  if (!up)
    return 0;

  uint16_t subId = 0;
  auto sub = std::make_shared<Subscriber>();
  {
    std::lock_guard<std::mutex> hubLock(hubMutex_);
    for (int attempt = 0; attempt < 65535; ++attempt) {
      subId = nextSubId_++;
      if (nextSubId_ == 0)
        nextSubId_ = 1;
      if (subId != 0 && subToUpstream_.find(subId) == subToUpstream_.end())
        break;
      subId = 0;
    }
    if (subId == 0) {
      error = "no_free_subscription_id";
      return 0;
    }
    subToUpstream_[subId] = up;
  }

  sub->subId = subId;
  sub->sink = input.sink;
  {
    std::lock_guard<std::mutex> upLock(up->mtx);
    up->subs.push_back(sub);
  }
  return subId;
}

void StreamHub::ack(uint16_t subId, int64_t bytes)
{
  if (bytes <= 0)
    return;
  std::shared_ptr<Upstream> up;
  {
    std::lock_guard<std::mutex> hubLock(hubMutex_);
    const auto it = subToUpstream_.find(subId);
    if (it == subToUpstream_.end())
      return;
    up = it->second;
  }

  std::shared_ptr<ISink> sink;
  {
    std::lock_guard<std::mutex> upLock(up->mtx);
    for (auto& sub : up->subs) {
      if (sub->subId == subId) {
        sink = sub->sink;
        break;
      }
    }
  }
  if (sink)
    sink->release(bytes);
}

void StreamHub::unsubscribe(uint16_t subId)
{
  std::shared_ptr<Upstream> up;
  {
    std::lock_guard<std::mutex> hubLock(hubMutex_);
    const auto it = subToUpstream_.find(subId);
    if (it == subToUpstream_.end())
      return;
    up = it->second;
    subToUpstream_.erase(it);
  }

  std::lock_guard<std::mutex> upLock(up->mtx);
  up->subs.erase(std::remove_if(up->subs.begin(), up->subs.end(),
                                [&](const auto& s) {
                                  return s->subId == subId;
                                }),
                 up->subs.end());
}

void StreamHub::closeAll(const ISink* sink)
{
  std::vector<std::shared_ptr<Upstream>> ups;
  {
    std::lock_guard<std::mutex> hubLock(hubMutex_);
    for (const auto& [name, up] : upstreams_)
      ups.push_back(up);
  }

  for (auto& up : ups) {
    std::lock_guard<std::mutex> upLock(up->mtx);
    up->subs.erase(std::remove_if(up->subs.begin(), up->subs.end(),
                                  [&](const auto& s) {
                                    return s->sink.get() == sink;
                                  }),
                   up->subs.end());
  }
}

int StreamHub::activeUpstreams()
{
  std::lock_guard<std::mutex> lock(hubMutex_);
  int count = 0;
  for (const auto& [name, up] : upstreams_)
    if (!up->dead.load(std::memory_order_acquire))
      ++count;
  return count;
}

int StreamHub::activeSubscribers()
{
  std::lock_guard<std::mutex> hubLock(hubMutex_);
  int total = 0;
  for (const auto& [name, up] : upstreams_) {
    std::lock_guard<std::mutex> upLock(up->mtx);
    total += static_cast<int>(up->subs.size());
  }
  return total;
}
