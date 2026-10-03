#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/stream/gop-cache.hxx>
#include <shared/services/stream/upstream-http.hxx>

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
  return {.bytes = std::make_shared<const std::string>(size, 'x'), .kind = kind};
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
