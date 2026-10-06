#include "stream-hub.hxx"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <drogon/drogon.h>
#include <runtime/blocking-pool.hxx>
#include <config/config-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/upstream-http.hxx>
#include <shared/services/stream/ws-frame.hxx>
#include <shared/services/privacy/camera-audio-policy.hxx>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
constexpr size_t kDefaultChunkBytes = 16 * 1024;
constexpr int64_t kDefaultGraceMs = 2000;
constexpr int64_t kStallMs = 10000;
constexpr int64_t kFreshGopMs = 3000;
constexpr size_t kReadBytes = size_t{64} * 1024;

int64_t steadyNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}

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
  thread_local std::string blob;
  ws_frame::frameInto(blob, {.header = h, .payload = input.data, .len = input.len});
  if (!sub->sink->sendBinary(reinterpret_cast<const uint8_t*>(blob.data()),
                             blob.size())) {
    sub->sink = nullptr;
  }
}

void StreamHub::deliver(const DeliverInput& input)
{
  const std::shared_ptr<Subscriber>& sub = input.sub;
  if (!sub->sink)
    return;
  const auto kind = input.fragment.kind;
  const bool video = kind != upstream_http::FragmentKind::Other;
  const bool keyframe = kind == upstream_http::FragmentKind::VideoKey;
  const auto admit = [&input, &sub](size_t bytes) {
    return input.reserved || sub->sink->tryReserve(bytes);
  };
  if (sub->skipUntilKeyframe) {
    if (!keyframe)
      return;
    if (!sub->sentInit && input.up.hasInit) {
      if (!admit(input.up.init.size()))
        return;
      sendFramed({.sub = sub,
                  .type = ws_frame::kTypeInit,
                  .keyframe = true,
                  .data = reinterpret_cast<const uint8_t*>(input.up.init.data()),
                  .len = input.up.init.size()});
      if (!sub->sink)
        return;
      sub->sentInit = true;
    }
    sub->skipUntilKeyframe = false;
  }

  const std::string& box = *input.fragment.bytes;
  if (!admit(box.size())) {
    if (video)
      sub->skipUntilKeyframe = true;
    return;
  }
  size_t offset = 0;
  bool first = true;
  while (offset < box.size()) {
    const size_t part = std::min(chunkBytes_, box.size() - offset);
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

void StreamHub::dispatch(Upstream& up, const CachedFragment& fragment)
{
  std::scoped_lock lock(up.mtx);
  up.gop.add(fragment);
  for (const auto& sub : up.subs)
    deliver({.up = up, .sub = sub, .fragment = fragment, .reserved = false});
  std::erase_if(up.subs, [](const auto& s) { return !s->sink; });
}

void StreamHub::replayGop(Upstream& up, const std::shared_ptr<Subscriber>& sub)
{
  const auto& fragments = up.gop.fragments();
  if (!up.gop.freshAt({.nowMs = steadyNowMs(), .maxAgeMs = kFreshGopMs}) ||
      !up.hasInit || !sub->sink)
    return;
  const bool reserved = sub->sink->tryReserve(up.init.size() + up.gop.bytes());
  for (const CachedFragment& fragment : fragments)
    deliver({.up = up, .sub = sub, .fragment = fragment, .reserved = reserved});
}

void StreamHub::runUpstream(std::shared_ptr<Upstream> up)
{
  const auto [host, port] = upstream_http::splitHostPort(
      Go2rtcManager::instance().apiBase().substr(7));
  const std::string path =
      "/api/stream.mp4?src=" + up->name +
      (CameraAudioPolicy::instance().allowed() ? "&mp4=flac" : "&video");

  upstream_http::Upstream conn =
      upstream_http::open({.host = host, .port = port, .path = path,
                           .timeoutSec = 10, .cancel = &up->stopping});
  if (!conn.ok) {
    LOG_WARN << "StreamHub: upstream failed for " << up->name;
    std::scoped_lock lock(up->mtx);
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
    std::scoped_lock lock(up->mtx);
    up->init = std::move(box);
    up->hasInit = true;
  };
  reader.onFragment = [this, &up](upstream_http::Fmp4Fragment fragment) {
    dispatch(*up,
             {.bytes = std::make_shared<const std::string>(std::move(fragment.bytes)),
              .kind = fragment.kind,
              .arrivedMs = steadyNowMs()});
  };
  if (!conn.leftover.empty())
    reader.feed(conn.leftover.data(), conn.leftover.size());

  int64_t emptySinceMs = 0;
  int64_t lastDataMs = steadyNowMs();
  std::string closeReason = "upstream_closed";
  std::vector<char> buf(kReadBytes);
  while (!up->stopping.load(std::memory_order_relaxed)) {
    const ssize_t n = ::recv(conn.fd, buf.data(), buf.size(), 0);
    if (n > 0) {
      reader.feed(buf.data(), static_cast<size_t>(n));
      lastDataMs = steadyNowMs();
      if (reader.corrupt()) {
        LOG_WARN << "StreamHub: " << up->name << " sent a malformed MP4 box; reconnecting";
        closeReason = "upstream_corrupt";
        break;
      }
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
      std::scoped_lock lock(up->mtx);
      empty = up->subs.empty();
    }
    const int64_t nowMs = steadyNowMs();
    if (empty) {
      if (emptySinceMs == 0)
        emptySinceMs = nowMs;
      else if (nowMs - emptySinceMs >= graceMs_)
        break;
    }
    else {
      emptySinceMs = 0;
      if (nowMs - lastDataMs >= kStallMs) {
        LOG_WARN << "StreamHub: " << up->name << " sent nothing for "
                 << nowMs - lastDataMs << " ms; closing it";
        closeReason = "upstream_stalled";
        break;
      }
    }
  }

  if (const int fd = up->fd.exchange(-1); fd >= 0)
    ::close(fd);
  std::scoped_lock lock(up->mtx);
  for (auto& sub : up->subs) {
    if (sub->sink)
      sub->sink->onClosed({.subId = sub->subId, .reason = closeReason});
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
  if (const int64_t v = ConfigService::getInt("streaming.hub_gop_cache_bytes");
      v > 0)
    gopCacheBytes_ = static_cast<size_t>(v);
  refreshViewerLimits();
  const ViewerLimits limits = viewerLimits();
  LOG_INFO << "StreamHub ready (chunk=" << chunkBytes_ << "B grace=" << graceMs_
           << "ms gop-cache=" << gopCacheBytes_
           << "B viewers/camera=" << limits.perCamera
           << " viewers/total=" << limits.total << ")";
}

void StreamHub::refreshViewerLimits()
{
  const auto configured = [](const std::string& key, int fallback) {
    const int value = ConfigService::getInt(key);
    return value > 0 ? value : fallback;
  };
  const ViewerLimits limits{
      .perCamera = configured("streaming.max_viewers_per_camera",
                              kDefaultViewersPerCamera),
      .total = configured("streaming.max_total_viewers", kDefaultTotalViewers)};
  std::scoped_lock lock(hubMutex_);
  maxViewersPerCamera_ = limits.perCamera;
  maxTotalViewers_ = limits.total;
}

StreamHub::ViewerLimits StreamHub::viewerLimits()
{
  std::scoped_lock lock(hubMutex_);
  return {.perCamera = maxViewersPerCamera_, .total = maxTotalViewers_};
}

void StreamHub::shutdown()
{
  std::vector<std::thread> readers;
  {
    std::scoped_lock lock(hubMutex_);
    for (auto& [name, up] : upstreams_) {
      up->stopping.store(true, std::memory_order_relaxed);
      if (up->reader.joinable())
        readers.push_back(std::move(up->reader));
    }
    upstreams_.clear();
    subToUpstream_.clear();
    for (auto& up : retired_) {
      if (up->reader.joinable())
        readers.push_back(std::move(up->reader));
    }
    retired_.clear();
  }
  for (auto& reader : readers)
    reader.join();
}

std::vector<std::thread> StreamHub::collectFinishedLocked()
{
  std::vector<std::thread> finished;
  std::erase_if(retired_, [&finished](const std::shared_ptr<Upstream>& up) {
    if (!up->dead.load(std::memory_order_acquire))
      return false;
    if (up->reader.joinable())
      finished.push_back(std::move(up->reader));
    return true;
  });
  return finished;
}

void StreamHub::reap(std::vector<std::thread> readers)
{
  if (readers.empty())
    return;
  auto shared = std::make_shared<std::vector<std::thread>>(std::move(readers));
  blocking_pool::submit(BlockingLane::Light, [shared]() {
    for (auto& reader : *shared)
      if (reader.joinable())
        reader.join();
  });
}

void StreamHub::restartUpstreams()
{
  std::scoped_lock lock(hubMutex_);
  for (auto& [name, up] : upstreams_)
    up->stopping.store(true, std::memory_order_release);
}

std::shared_ptr<StreamHub::Upstream>
StreamHub::getOrOpen(const SubscribeInput& input, std::string& error)
{
  if (!Go2rtcManager::instance().isRunning()) {
    error = "go2rtc_not_running";
    return nullptr;
  }

  const std::string name = Go2rtcManager::sourceName(input.cameraId, input.stream);
  std::scoped_lock lock(hubMutex_);
  auto it = upstreams_.find(name);
  if (it != upstreams_.end()) {
    if (!it->second->dead.load(std::memory_order_acquire) &&
        !it->second->stopping.load(std::memory_order_acquire))
      return it->second;
    it->second->stopping.store(true, std::memory_order_release);
    retired_.push_back(it->second);
    upstreams_.erase(it);
  }

  auto opened = std::make_shared<Upstream>(gopCacheBytes_);
  opened->name = name;
  opened->cameraId = input.cameraId;
  opened->reader = std::thread(&StreamHub::runUpstream, this, opened);
  upstreams_.emplace(name, opened);
  return opened;
}

void StreamHub::pruneLocked()
{
  std::erase_if(subToUpstream_, [](const auto& entry) {
    const auto& [subId, up] = entry;
    std::scoped_lock upLock(up->mtx);
    return std::ranges::none_of(up->subs, [subId](const auto& sub) {
      return sub->subId == subId;
    });
  });
  std::erase_if(upstreams_, [this](auto& entry) {
    if (!entry.second->dead.load(std::memory_order_acquire))
      return false;
    retired_.push_back(entry.second);
    return true;
  });
}

StreamHub::ViewerCount StreamHub::countViewers(int64_t cameraId)
{
  ViewerCount count;
  for (const auto& [name, up] : upstreams_) {
    std::scoped_lock upLock(up->mtx);
    const int viewers = static_cast<int>(up->subs.size());
    count.total += viewers;
    if (up->cameraId == cameraId)
      count.perCamera += viewers;
  }
  return count;
}

const char* StreamHub::viewerRefusal(const SubscribeInput& input)
{
  const auto ceiling = [&input](int limit) {
    return input.priority || limit <= 1 ? limit : limit - 1;
  };
  const ViewerCount count = countViewers(input.cameraId);
  if (maxTotalViewers_ > 0 && count.total >= ceiling(maxTotalViewers_))
    return "too_many_viewers";
  if (maxViewersPerCamera_ > 0 && count.perCamera >= ceiling(maxViewersPerCamera_))
    return "too_many_viewers_for_camera";
  return nullptr;
}

uint16_t StreamHub::subscribe(const SubscribeInput& input, std::string& error)
{
  if (!input.sink) {
    error = "invalid_sink";
    return 0;
  }

  std::vector<std::thread> finished;
  {
    std::scoped_lock hubLock(hubMutex_);
    pruneLocked();
    finished = collectFinishedLocked();
    if (const char* refusal = viewerRefusal(input)) {
      error = refusal;
      reap(std::move(finished));
      return 0;
    }
  }
  reap(std::move(finished));

  auto up = getOrOpen(input, error);
  if (!up)
    return 0;

  auto sub = std::make_shared<Subscriber>();
  {
    std::scoped_lock hubLock(hubMutex_);
    if (const char* refusal = viewerRefusal(input)) {
      error = refusal;
      return 0;
    }
    uint16_t subId = 0;
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
    sub->subId = subId;
    sub->sink = input.sink;
    {
      std::scoped_lock upLock(up->mtx);
      up->subs.push_back(sub);
      if (input.fastStart)
        replayGop(*up, sub);
    }
    subToUpstream_[subId] = up;
  }
  return sub->subId;
}

int64_t StreamHub::ack(const AckInput& input)
{
  if (input.bytes <= 0 || input.owner == nullptr)
    return 0;
  std::shared_ptr<Upstream> up;
  {
    std::scoped_lock hubLock(hubMutex_);
    const auto it = subToUpstream_.find(input.subId);
    if (it == subToUpstream_.end())
      return 0;
    up = it->second;
  }

  std::shared_ptr<ISink> sink;
  {
    std::scoped_lock upLock(up->mtx);
    const auto sub = std::ranges::find(up->subs, input.subId, &Subscriber::subId);
    if (sub != up->subs.end())
      sink = (*sub)->sink;
  }
  if (!sink || sink.get() != input.owner)
    return 0;
  return sink->release(input.bytes);
}

void StreamHub::unsubscribe(uint16_t subId, const ISink* owner)
{
  std::scoped_lock hubLock(hubMutex_);
  const auto it = subToUpstream_.find(subId);
  if (it == subToUpstream_.end())
    return;
  Upstream& up = *it->second;
  std::scoped_lock upLock(up.mtx);
  const auto sub = std::ranges::find(up.subs, subId, &Subscriber::subId);
  if (sub != up.subs.end()) {
    if ((*sub)->sink && (*sub)->sink.get() != owner)
      return;
    up.subs.erase(sub);
  }
  subToUpstream_.erase(it);
}

int StreamHub::subscriptionsOf(const ISink* sink)
{
  std::scoped_lock hubLock(hubMutex_);
  int count = 0;
  for (const auto& [name, up] : upstreams_) {
    std::scoped_lock upLock(up->mtx);
    count += static_cast<int>(std::ranges::count_if(
        up->subs, [sink](const auto& sub) { return sub->sink.get() == sink; }));
  }
  return count;
}

void StreamHub::closeAll(const ISink* sink)
{
  std::vector<std::shared_ptr<Upstream>> ups;
  {
    std::scoped_lock hubLock(hubMutex_);
    for (const auto& [name, up] : upstreams_)
      ups.push_back(up);
  }

  for (auto& up : ups) {
    std::scoped_lock upLock(up->mtx);
    up->subs.erase(std::remove_if(up->subs.begin(), up->subs.end(),
                                  [&](const auto& s) {
                                    return s->sink.get() == sink;
                                  }),
                   up->subs.end());
  }
}

int StreamHub::activeUpstreams()
{
  std::scoped_lock lock(hubMutex_);
  int count = 0;
  for (const auto& [name, up] : upstreams_)
    if (!up->dead.load(std::memory_order_acquire))
      ++count;
  return count;
}

std::unordered_map<int64_t, int> StreamHub::viewersByCamera()
{
  std::scoped_lock hubLock(hubMutex_);
  std::unordered_map<int64_t, int> viewers;
  for (const auto& [name, up] : upstreams_) {
    std::scoped_lock upLock(up->mtx);
    viewers[up->cameraId] += static_cast<int>(up->subs.size());
  }
  return viewers;
}

int StreamHub::activeSubscribers()
{
  std::scoped_lock hubLock(hubMutex_);
  int total = 0;
  for (const auto& [name, up] : upstreams_) {
    std::scoped_lock upLock(up->mtx);
    total += static_cast<int>(up->subs.size());
  }
  return total;
}
