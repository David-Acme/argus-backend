#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/gop-cache.hxx>
#include <shared/services/stream/grab-backoff.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <shared/services/stream/upstream-http.hxx>
#include <shared/services/stream/viewer-slots.hxx>
#include <shared/services/stream/ws-frame.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
using upstream_http::FragmentKind;

constexpr uint32_t kSyncSample = 0x02000000U;
constexpr uint32_t kDeltaSample = 0x01010000U;

std::string be32(uint32_t value)
{
  return {static_cast<char>((value >> 24) & 0xFF), static_cast<char>((value >> 16) & 0xFF),
          static_cast<char>((value >> 8) & 0xFF), static_cast<char>(value & 0xFF)};
}

struct BoxSpec
{
  std::string_view type;
  std::string body;
};

std::string box(const BoxSpec& spec)
{
  return be32(static_cast<uint32_t>(spec.body.size() + 8)) + std::string(spec.type) + spec.body;
}

std::string trak(uint32_t trackId, std::string_view handler)
{
  const std::string tkhd = box({.type = "tkhd", .body = be32(0) + be32(0) + be32(0) + be32(trackId) + be32(0)});
  const std::string hdlr = box({.type = "hdlr", .body = be32(0) + be32(0) + std::string(handler) + be32(0)});
  const std::string mdia =
      box({.type = "mdia", .body = box({.type = "mdhd", .body = be32(0)}) + hdlr});
  return box({.type = "trak", .body = tkhd + mdia});
}

std::string initSegment(const std::vector<std::string>& traks)
{
  std::string body = box({.type = "mvhd", .body = be32(0)});
  for (const auto& entry : traks)
    body += entry;
  return box({.type = "ftyp", .body = "iso5"}) + box({.type = "moov", .body = body});
}

struct FragmentSpec
{
  uint32_t track{1};
  uint32_t defaultFlags{kDeltaSample};
  uint32_t trunFlags{0x000001};
  uint32_t trunValue{0};
};

std::string fragment(const FragmentSpec& spec)
{
  const std::string tfhd = box({.type = "tfhd", .body = be32(0x020038) + be32(spec.track) + be32(3000) +
                                           be32(4) + be32(spec.defaultFlags)});
  std::string trun = be32(spec.trunFlags) + be32(1) + be32(0);
  if (spec.trunFlags & 0x000004U)
    trun += be32(spec.trunValue);
  if (spec.trunFlags & 0x000400U)
    trun += be32(spec.trunValue);
  const std::string tfdt = box({.type = "tfdt", .body = be32(0) + be32(0)});
  const std::string traf =
      box({.type = "traf", .body = tfhd + tfdt + box({.type = "trun", .body = trun})});
  const std::string mfhd = box({.type = "mfhd", .body = be32(0) + be32(1)});
  return box({.type = "moof", .body = mfhd + traf}) + box({.type = "mdat", .body = "abcd"});
}

struct Collected
{
  std::string init;
  std::vector<upstream_http::Fmp4Fragment> fragments;
};

upstream_http::Fmp4Reader collectingReader(Collected& collected, bool chunked)
{
  upstream_http::Fmp4Reader reader({.chunked = chunked});
  reader.onInit = [&collected](std::string init) { collected.init = std::move(init); };
  reader.onFragment = [&collected](upstream_http::Fmp4Fragment fragment) {
    collected.fragments.push_back(std::move(fragment));
  };
  return reader;
}

CachedFragment cached(FragmentKind kind, size_t size)
{
  return {.bytes = std::make_shared<const std::string>(size, 'x'), .kind = kind, .arrivedMs = 1000};
}
}

TEST_CASE("the video track is found by its handler, wherever it sits in moov")
{
  CHECK(upstream_http::videoTrackOf(initSegment({trak(1, "vide"), trak(2, "soun")})) == 1);
  CHECK(upstream_http::videoTrackOf(initSegment({trak(1, "soun"), trak(2, "vide")})) == 2);
  CHECK(upstream_http::videoTrackOf(initSegment({trak(1, "soun")})) == 0);
  CHECK(upstream_http::videoTrackOf("") == 0);
}

TEST_CASE("an audio fragment flagged sync is never a video keyframe")
{
  Collected collected;
  auto reader = collectingReader(collected, false);
  const std::string stream =
      initSegment({trak(1, "vide"), trak(2, "soun")}) +
      fragment({.track = 2, .defaultFlags = kSyncSample}) +
      fragment({.track = 1, .defaultFlags = kDeltaSample}) +
      fragment({.track = 1, .defaultFlags = kSyncSample});

  for (const char byte : stream)
    reader.feed(&byte, 1);

  CHECK_FALSE(collected.init.empty());
  REQUIRE(collected.fragments.size() == 3);
  CHECK(collected.fragments[0].kind == FragmentKind::Other);
  CHECK(collected.fragments[1].kind == FragmentKind::VideoDelta);
  CHECK(collected.fragments[2].kind == FragmentKind::VideoKey);
}

TEST_CASE("trun sample flags override the tfhd default")
{
  const uint32_t video = 1;
  CHECK(upstream_http::fragmentKindOf(
            {.moof = fragment({.track = video,
                               .defaultFlags = kDeltaSample,
                               .trunFlags = 0x000005,
                               .trunValue = kSyncSample}),
             .videoTrack = video}) == FragmentKind::VideoKey);
  CHECK(upstream_http::fragmentKindOf(
            {.moof = fragment({.track = video,
                               .defaultFlags = kSyncSample,
                               .trunFlags = 0x000401,
                               .trunValue = kDeltaSample}),
             .videoTrack = video}) == FragmentKind::VideoDelta);
}

TEST_CASE("without an identified video track the sample flags decide")
{
  CHECK(upstream_http::fragmentKindOf(
            {.moof = fragment({.track = 5, .defaultFlags = kSyncSample}), .videoTrack = 0}) ==
        FragmentKind::VideoKey);
  CHECK(upstream_http::fragmentKindOf(
            {.moof = fragment({.track = 5, .defaultFlags = kDeltaSample}), .videoTrack = 0}) ==
        FragmentKind::VideoDelta);
}

TEST_CASE("chunked transfer encoding is unwrapped before boxes are read")
{
  Collected collected;
  auto reader = collectingReader(collected, true);
  const std::string payload = initSegment({trak(1, "vide")}) +
                              fragment({.track = 1, .defaultFlags = kSyncSample});
  const std::string half = payload.substr(0, payload.size() / 2);
  const std::string rest = payload.substr(payload.size() / 2);
  const auto hex = [](size_t value) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    do {
      out.insert(out.begin(), kDigits[value % 16]);
      value /= 16;
    } while (value > 0);
    return out;
  };
  const std::string wire =
      hex(half.size()) + "\r\n" + half + "\r\n" + hex(rest.size()) + "\r\n" + rest + "\r\n0\r\n\r\n";

  reader.feed(wire.data(), wire.size());

  REQUIRE(collected.fragments.size() == 1);
  CHECK(collected.fragments[0].kind == FragmentKind::VideoKey);
}

TEST_CASE("a 64-bit box header waits for its full sixteen bytes")
{
  Collected collected;
  auto reader = collectingReader(collected, false);
  const std::string partial = be32(1) + "mdat" + be32(0);
  reader.feed(partial.data(), partial.size());
  CHECK(collected.init.empty());
  CHECK(collected.fragments.empty());
}

TEST_CASE("the gop cache starts at a video keyframe and holds what follows it")
{
  GopCache cache(1000);
  cache.add(cached(FragmentKind::Other, 10));
  cache.add(cached(FragmentKind::VideoDelta, 10));
  CHECK(cache.fragments().empty());

  cache.add(cached(FragmentKind::VideoKey, 100));
  cache.add(cached(FragmentKind::Other, 10));
  cache.add(cached(FragmentKind::VideoDelta, 20));
  REQUIRE(cache.fragments().size() == 3);
  CHECK(cache.fragments().front().kind == FragmentKind::VideoKey);
  CHECK(cache.bytes() == 130);

  cache.add(cached(FragmentKind::VideoKey, 50));
  REQUIRE(cache.fragments().size() == 1);
  CHECK(cache.bytes() == 50);
}

TEST_CASE("a gop larger than the cache is dropped until the next keyframe")
{
  GopCache cache(100);
  cache.add(cached(FragmentKind::VideoKey, 60));
  cache.add(cached(FragmentKind::VideoDelta, 60));
  CHECK(cache.fragments().empty());
  cache.add(cached(FragmentKind::VideoDelta, 10));
  CHECK(cache.fragments().empty());
  cache.add(cached(FragmentKind::VideoKey, 40));
  CHECK(cache.fragments().size() == 1);
}

TEST_CASE("a zero-capacity gop cache holds nothing")
{
  GopCache cache(0);
  cache.add(cached(FragmentKind::VideoKey, 10));
  CHECK(cache.fragments().empty());
  CHECK(cache.bytes() == 0);
}

TEST_CASE("a gop is fresh only while its last fragment is recent")
{
  GopCache cache(1000);
  CHECK_FALSE(cache.freshAt({.nowMs = 1000, .maxAgeMs = 3000}));
  cache.add(cached(FragmentKind::VideoKey, 10));
  CHECK(cache.freshAt({.nowMs = 1000, .maxAgeMs = 3000}));
  CHECK(cache.freshAt({.nowMs = 4000, .maxAgeMs = 3000}));
  CHECK_FALSE(cache.freshAt({.nowMs = 4001, .maxAgeMs = 3000}));
}

TEST_CASE("a malformed box marks the reader corrupt instead of parsing garbage")
{
  Collected collected;
  auto reader = collectingReader(collected, false);
  const std::string wire = initSegment({trak(1, "vide")}) + be32(4) + "moof" +
                           fragment({.track = 1, .defaultFlags = kSyncSample, .trunFlags = 0x000001U, .trunValue = 0});
  reader.feed(wire.data(), wire.size());
  CHECK_FALSE(collected.init.empty());
  CHECK(reader.corrupt());
  CHECK(collected.fragments.empty());
  reader.feed(wire.data(), wire.size());
  CHECK(collected.fragments.empty());
  reader.reset();
  CHECK_FALSE(reader.corrupt());
}

TEST_CASE("an init segment that never reaches moov is bounded")
{
  Collected collected;
  auto reader = collectingReader(collected, false);
  const std::string filler = box({.type = "free", .body = std::string(std::size_t{1024} * 1024, 'f')});
  for (int i = 0; i < 6 && !reader.corrupt(); ++i)
    reader.feed(filler.data(), filler.size());
  CHECK(reader.corrupt());
  CHECK(collected.init.empty());
}

TEST_CASE("fragments split across many feeds come out whole and in order")
{
  Collected collected;
  auto reader = collectingReader(collected, false);
  std::string wire = initSegment({trak(1, "vide")});
  for (int i = 0; i < 20; ++i)
    wire += fragment({.track = 1, .defaultFlags = i % 5 == 0 ? kSyncSample : kDeltaSample,
                      .trunFlags = 0x000001U, .trunValue = 0});
  for (size_t offset = 0; offset < wire.size(); offset += 7)
    reader.feed(wire.data() + offset, std::min<size_t>(7, wire.size() - offset));
  REQUIRE(collected.fragments.size() == 20);
  CHECK(collected.fragments[0].kind == FragmentKind::VideoKey);
  CHECK(collected.fragments[1].kind == FragmentKind::VideoDelta);
  CHECK(collected.fragments[5].kind == FragmentKind::VideoKey);
  CHECK(collected.fragments[19].bytes ==
        fragment({.track = 1, .defaultFlags = kDeltaSample, .trunFlags = 0x000001U, .trunValue = 0}));
}

TEST_CASE("only an HTTP 200 status line opens an upstream")
{
  CHECK(upstream_http::isOkStatusLine("HTTP/1.1 200 OK\r\nContent-Type: video/mp4"));
  CHECK(upstream_http::isOkStatusLine("HTTP/1.0 200"));
  CHECK_FALSE(upstream_http::isOkStatusLine("HTTP/1.1 500 Internal\r\nX-Note: 200"));
  CHECK_FALSE(upstream_http::isOkStatusLine("HTTP/1.1 2000 Odd"));
  CHECK_FALSE(upstream_http::isOkStatusLine("RTSP/1.0 200 OK"));
}

TEST_CASE("opening an upstream gives up at once when its stream is stopping")
{
  const std::atomic<bool> stopping{true};
  const auto up = upstream_http::open(
      {.host = "10.255.255.1", .port = 9, .path = "/", .timeoutSec = 10, .cancel = &stopping});
  CHECK_FALSE(up.ok);
  CHECK(up.fd == -1);
}

TEST_CASE("a failing camera backs off, reports the first failure of the episode and starts over on a frame")
{
  GrabBackoff backoff;
  const GrabAttempt first = backoff.begin({.cameraId = 9, .nowMs = 1000});
  CHECK(first.attempt);
  CHECK(first.firstFailure);
  CHECK(first.retryDelayMs == 1000);
  backoff.noteFailure({.cameraId = 9, .nowMs = 1000});

  const GrabAttempt held = backoff.begin({.cameraId = 9, .nowMs = 1500});
  CHECK_FALSE(held.attempt);
  CHECK_FALSE(held.firstFailure);

  const GrabAttempt retry = backoff.begin({.cameraId = 9, .nowMs = 2000});
  CHECK(retry.attempt);
  CHECK_FALSE(retry.firstFailure);
  CHECK(retry.retryDelayMs == 2000);
  backoff.noteFailure({.cameraId = 9, .nowMs = 2000});

  CHECK_FALSE(backoff.begin({.cameraId = 9, .nowMs = 3000}).attempt);
  const GrabAttempt third = backoff.begin({.cameraId = 9, .nowMs = 4000});
  CHECK(third.attempt);
  CHECK(third.retryDelayMs == 4000);
  backoff.noteFailure({.cameraId = 9, .nowMs = 4000});

  backoff.noteSuccess(9);
  const GrabAttempt recovered = backoff.begin({.cameraId = 9, .nowMs = 5000});
  CHECK(recovered.attempt);
  CHECK(recovered.firstFailure);
  CHECK(recovered.retryDelayMs == 1000);
}

TEST_CASE("the camera backoff doubles to half a minute")
{
  GrabBackoff backoff;
  int64_t now = 0;
  for (const int64_t expected : {1000, 2000, 4000, 8000, 16000, 30000, 30000}) {
    const GrabAttempt step = backoff.begin({.cameraId = 3, .nowMs = now});
    REQUIRE(step.attempt);
    CHECK(step.retryDelayMs == expected);
    backoff.noteFailure({.cameraId = 3, .nowMs = now});
    now += expected;
  }
}

TEST_CASE("a recorder rides beside the four viewers of its camera")
{
  const viewer_slots::Limits limits{.perCamera = 4, .total = 8};
  const std::vector<viewer_slots::Viewer> viewers{
      {.cameraId = 4, .recorder = false},
      {.cameraId = 4, .recorder = false},
      {.cameraId = 4, .recorder = false},
      {.cameraId = 4, .recorder = false},
      {.cameraId = 4, .recorder = true},
  };

  const viewer_slots::Occupancy occupancy = viewer_slots::occupancyOf(viewers, 4);
  CHECK(occupancy.perCamera == 4);
  CHECK(occupancy.total == 4);

  CHECK(viewer_slots::refusalOf(limits, occupancy,
                                {.recorder = true, .priority = false}) ==
        viewer_slots::Refusal::None);
  CHECK(viewer_slots::refusalOf(limits, occupancy,
                                {.recorder = true, .priority = true}) ==
        viewer_slots::Refusal::None);
  CHECK(viewer_slots::refusalOf(limits, occupancy,
                                {.recorder = false, .priority = false}) ==
        viewer_slots::Refusal::Camera);
  CHECK(viewer_slots::refusalOf(limits, occupancy,
                                {.recorder = false, .priority = true}) ==
        viewer_slots::Refusal::Camera);
  CHECK(std::string(viewer_slots::refusalMessage(viewer_slots::Refusal::Camera)) ==
        "too_many_viewers_for_camera");
  CHECK(viewer_slots::refusalMessage(viewer_slots::Refusal::None) == nullptr);
}

TEST_CASE("the total budget refuses a viewer before the camera budget does")
{
  const viewer_slots::Limits limits{.perCamera = 4, .total = 8};
  const std::vector<viewer_slots::Viewer> viewers{
      {.cameraId = 1, .recorder = true},
      {.cameraId = 2, .recorder = true},
      {.cameraId = 3, .recorder = false},
      {.cameraId = 4, .recorder = false},
      {.cameraId = 5, .recorder = false},
      {.cameraId = 6, .recorder = false},
      {.cameraId = 7, .recorder = false},
      {.cameraId = 8, .recorder = false},
      {.cameraId = 9, .recorder = false},
      {.cameraId = 10, .recorder = false},
  };

  const viewer_slots::Occupancy occupancy = viewer_slots::occupancyOf(viewers, 3);
  CHECK(occupancy.perCamera == 1);
  CHECK(occupancy.total == 8);

  CHECK(viewer_slots::refusalOf(limits, occupancy,
                                {.recorder = false, .priority = true}) ==
        viewer_slots::Refusal::Total);
  CHECK(viewer_slots::refusalOf(limits, occupancy,
                                {.recorder = true, .priority = false}) ==
        viewer_slots::Refusal::None);
}

TEST_CASE("the per-camera budget keeps a priority slot free and holds at one")
{
  const std::vector<viewer_slots::Viewer> three{
      {.cameraId = 2, .recorder = false},
      {.cameraId = 2, .recorder = false},
      {.cameraId = 2, .recorder = false},
  };
  const viewer_slots::Occupancy occupancy = viewer_slots::occupancyOf(three, 2);
  CHECK(occupancy.perCamera == 3);

  CHECK(viewer_slots::refusalOf({.perCamera = 4, .total = 8}, occupancy,
                                {.recorder = false, .priority = false}) ==
        viewer_slots::Refusal::Camera);
  CHECK(viewer_slots::refusalOf({.perCamera = 4, .total = 8}, occupancy,
                                {.recorder = false, .priority = true}) ==
        viewer_slots::Refusal::None);
  CHECK(viewer_slots::refusalOf({.perCamera = 1, .total = 8}, occupancy,
                                {.recorder = false, .priority = false}) ==
        viewer_slots::Refusal::Camera);
  CHECK(viewer_slots::refusalOf({.perCamera = 1, .total = 8},
                                viewer_slots::Occupancy{}, {.recorder = false,
                                                            .priority = false}) ==
        viewer_slots::Refusal::None);
}

namespace
{
int freePort()
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

std::string testDir(const std::string& tag)
{
  const std::string dir = "/tmp/argus-camera-stream-test-" + std::to_string(::getpid()) + "-" + tag;
  ::mkdir(dir.c_str(), 0700);
  return dir;
}

class FakeGo2rtc
{
public:
  explicit FakeGo2rtc(int port) : port_(port) {}

  void start()
  {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    ::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listenFd_, 4);
    thread_ = std::thread([this]() { serve(); });
  }

  void stop()
  {
    running_.store(false);
    ::shutdown(listenFd_, SHUT_RDWR);
    if (thread_.joinable())
      thread_.join();
    ::close(listenFd_);
    listenFd_ = -1;
  }

private:
  void serve()
  {
    const std::string body = initSegment({trak(1, "vide")}) +
                             fragment({.track = 1, .defaultFlags = kSyncSample}) +
                             fragment({.track = 1, .defaultFlags = kDeltaSample}) +
                             fragment({.track = 1, .defaultFlags = kDeltaSample}) +
                             fragment({.track = 1, .defaultFlags = kSyncSample});
    while (running_.load())
    {
      const int fd = ::accept(listenFd_, nullptr, nullptr);
      if (fd < 0)
        break;
      char buffer[2048];
      const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
      if (got <= 0)
      {
        ::close(fd);
        continue;
      }
      const std::string header =
          "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nConnection: close\r\n\r\n";
      ::send(fd, header.data(), header.size(), MSG_NOSIGNAL);
      ::send(fd, body.data(), body.size(), MSG_NOSIGNAL);
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      ::close(fd);
    }
  }

  int port_{0};
  int listenFd_{-1};
  std::atomic<bool> running_{true};
  std::thread thread_;
};

struct OrderingSink : StreamHub::ISink
{
  bool sendBinary(const uint8_t* data, size_t len) override
  {
    if (!subscribed.load())
      frameBeforeSubscribed.store(true);
    if (frames.fetch_add(1) == 0)
    {
      const auto header = ws_frame::parse({.data = data, .len = len});
      if (header.has_value())
        firstType.store(static_cast<int>(header->type));
    }
    return true;
  }
  bool tryReserve(size_t) override { return true; }
  int64_t release(int64_t bytes) override { return bytes; }
  void onSubscribed(uint16_t id, int64_t camera) override
  {
    subId.store(id);
    cameraId.store(camera);
    subscribed.store(true);
  }
  void onClosed(const StreamHub::StreamClosedInput&) override {}

  std::atomic<bool> subscribed{false};
  std::atomic<bool> frameBeforeSubscribed{false};
  std::atomic<int> frames{0};
  std::atomic<int> firstType{-1};
  std::atomic<uint16_t> subId{0};
  std::atomic<int64_t> cameraId{0};
};

bool waitFor(const std::atomic<int>& counter, int target, int maxMs)
{
  for (int waited = 0; waited < maxMs; waited += 20)
  {
    if (counter.load() >= target)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return counter.load() >= target;
}
}

TEST_CASE("a fastStart subscriber is told its subscription before the replayed frames reach it")
{
  const int apiPort = freePort();
  const std::string dir = testDir("hub");
  {
    std::ofstream stub(dir + "/stub-go2rtc.sh");
    stub << "#!/bin/sh\nsleep 30\n";
  }
  ::chmod((dir + "/stub-go2rtc.sh").c_str(), 0755);
  {
    std::ofstream config(dir + "/config.toml");
    config << "[streaming]\n"
           << "go2rtc_bin = \"" << dir << "/stub-go2rtc.sh\"\n"
           << "go2rtc_config = \"" << dir << "/go2rtc.yaml\"\n"
           << "go2rtc_api = \"127.0.0.1:" << apiPort << "\"\n"
           << "go2rtc_rtsp = \"127.0.0.1:" << freePort() << "\"\n"
           << "webrtc_listen = \"\"\n";
  }
  ConfigService::load(dir + "/config.toml");

  FakeGo2rtc server(apiPort);
  server.start();
  Go2rtcManager::instance().init();
  REQUIRE(Go2rtcManager::instance().isRunning());
  StreamHub::instance().init();

  auto first = std::make_shared<OrderingSink>();
  std::string error;
  const uint16_t firstId =
      StreamHub::instance().subscribe({.sink = first, .cameraId = 9, .fastStart = true}, error);
  REQUIRE(firstId != 0);
  REQUIRE(waitFor(first->frames, 1, 5000));
  CHECK(first->subscribed.load());
  CHECK_FALSE(first->frameBeforeSubscribed.load());
  CHECK(first->firstType.load() == static_cast<int>(ws_frame::kTypeInit));
  REQUIRE(waitFor(first->frames, 5, 5000));

  auto second = std::make_shared<OrderingSink>();
  const uint16_t secondId =
      StreamHub::instance().subscribe({.sink = second, .cameraId = 9, .fastStart = true}, error);
  REQUIRE(secondId != 0);
  CHECK(second->subscribed.load());
  CHECK_FALSE(second->frameBeforeSubscribed.load());
  CHECK(second->frames.load() > 0);
  CHECK(second->firstType.load() == static_cast<int>(ws_frame::kTypeInit));
  CHECK(second->cameraId.load() == 9);
  CHECK(second->subId.load() == secondId);

  StreamHub::instance().shutdown();
  Go2rtcManager::instance().shutdown();
  server.stop();
}
