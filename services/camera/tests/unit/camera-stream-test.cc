#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/stream/gop-cache.hxx>
#include <shared/services/stream/upstream-http.hxx>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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
