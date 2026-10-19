#include <doctest/doctest.h>

#include <feature/recording/infra/disk-space.hxx>
#include <feature/recording/services/clip-builder.hxx>
#include <feature/recording/services/clip-recorder.hxx>
#include <feature/recording/services/record-watchdog.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
std::string initBytes()
{
  return "moov" + std::string(20, 'x') + "avc1";
}

std::string chunkOf(std::size_t index)
{
  return "chunk-" + std::to_string(index) + "-" + std::string(4, 'c');
}

void pushChunk(ClipBuilder& builder, const std::string& bytes, bool keyframe,
               int64_t atMs)
{
  builder.push({.payload = reinterpret_cast<const uint8_t*>(bytes.data()),
                .len = bytes.size(),
                .keyframe = keyframe,
                .atMs = atMs});
}

ClipBuilderConfig testConfig()
{
  return {.preRollMs = 10000,
          .postRollMs = 10000,
          .segmentMs = 120000,
          .preRollBytes = size_t{1024} * 1024,
          .maxClipBytes = size_t{1024} * 1024};
}
}

TEST_CASE("triggerAllowed reads the record mode exactly")
{
  CHECK_FALSE(clip_recording::triggerAllowed(
      CameraRecordMode::Off, CameraClipTrigger::Event));
  CHECK_FALSE(clip_recording::triggerAllowed(
      CameraRecordMode::Off, CameraClipTrigger::Motion));
  CHECK(clip_recording::triggerAllowed(CameraRecordMode::Motion,
                                       CameraClipTrigger::Motion));
  CHECK_FALSE(clip_recording::triggerAllowed(
      CameraRecordMode::Motion, CameraClipTrigger::Event));
  CHECK(clip_recording::triggerAllowed(CameraRecordMode::Continuous,
                                       CameraClipTrigger::Event));
  CHECK(clip_recording::triggerAllowed(CameraRecordMode::Continuous,
                                       CameraClipTrigger::Motion));
  CHECK(clip_recording::triggerAllowed(CameraRecordMode::Events,
                                       CameraClipTrigger::Event));
  CHECK(clip_recording::triggerAllowed(CameraRecordMode::Events,
                                       CameraClipTrigger::Motion));
}

TEST_CASE("a clip opens at a keyframe covering the pre-roll and closes after the post-roll")
{
  ClipBuilder builder;
  builder.configure(testConfig());
  builder.setInit(initBytes());
  CHECK(builder.hasInit());
  CHECK(builder.codec() == "h264");

  for (int64_t index = 0; index < 20; ++index)
    pushChunk(builder, chunkOf(static_cast<std::size_t>(index)),
              index % 5 == 4, 1000 + index * 1000);
  CHECK_FALSE(builder.takeReady().has_value());

  const int64_t triggerMs = 20000;
  const int64_t epochMs = 1700000000000;
  CHECK(builder.request(CameraClipTrigger::Event, triggerMs, epochMs));
  CHECK(builder.active());

  pushChunk(builder, chunkOf(20), true, triggerMs + 1000);
  pushChunk(builder, chunkOf(21), false, triggerMs + 2000);
  CHECK_FALSE(builder.takeReady().has_value());
  pushChunk(builder, chunkOf(22), false, triggerMs + 11000);

  const auto ready = builder.takeReady();
  if (!ready.has_value())
  {
    FAIL("the clip never surfaced");
    return;
  }
  CHECK(ready->trigger == CameraClipTrigger::Event);
  CHECK(ready->codec == "h264");
  CHECK(ready->body.starts_with(initBytes()));
  CHECK(ready->body == initBytes() + chunkOf(9) + chunkOf(10) + chunkOf(11) +
                           chunkOf(12) + chunkOf(13) + chunkOf(14) +
                           chunkOf(15) + chunkOf(16) + chunkOf(17) +
                           chunkOf(18) + chunkOf(19) + chunkOf(20) +
                           chunkOf(21) + chunkOf(22));
  CHECK(ready->durationMs == 21000);
  CHECK(ready->startedAtMs == epochMs - 10000);
  CHECK_FALSE(builder.active());
  CHECK_FALSE(builder.takeReady().has_value());
}

TEST_CASE("a request with no keyframe in the ring waits for the next one")
{
  ClipBuilder builder;
  builder.configure(testConfig());
  builder.setInit(initBytes());

  pushChunk(builder, chunkOf(1), false, 5000);
  CHECK(builder.request(CameraClipTrigger::Event, 6000, 1700000000000));
  pushChunk(builder, chunkOf(2), false, 7000);
  CHECK_FALSE(builder.takeReady().has_value());
  CHECK(builder.active());

  pushChunk(builder, chunkOf(3), true, 8000);
  pushChunk(builder, chunkOf(4), false, 16000);

  const auto ready = builder.takeReady();
  if (!ready.has_value())
  {
    FAIL("the clip never surfaced on the next keyframe");
    return;
  }
  CHECK(ready->body == initBytes() + chunkOf(3) + chunkOf(4));
  CHECK(ready->startedAtMs == 1700000002000);
  CHECK(ready->durationMs == 8000);
}

TEST_CASE("motion reports extend the same clip instead of opening a second one")
{
  ClipBuilder builder;
  builder.configure(testConfig());
  builder.setInit(initBytes());

  const int64_t epochMs = 1700000000000;
  pushChunk(builder, chunkOf(1), true, 1000);
  CHECK(builder.request(CameraClipTrigger::Motion, 2000, epochMs));
  pushChunk(builder, chunkOf(2), false, 3000);
  CHECK_FALSE(builder.takeReady().has_value());
  CHECK(builder.request(CameraClipTrigger::Motion, 6000, 0));
  CHECK(builder.request(CameraClipTrigger::Motion, 12000, 0));
  pushChunk(builder, chunkOf(3), false, 13000);
  CHECK_FALSE(builder.takeReady().has_value());
  pushChunk(builder, chunkOf(4), false, 22000);

  const auto ready = builder.takeReady();
  if (!ready.has_value())
  {
    FAIL("the extended clip never surfaced");
    return;
  }
  CHECK(ready->trigger == CameraClipTrigger::Motion);
  CHECK(ready->body == initBytes() + chunkOf(1) + chunkOf(2) + chunkOf(3) + chunkOf(4));
  CHECK(ready->durationMs == 21000);
  CHECK(ready->startedAtMs == epochMs - 1000);
}

TEST_CASE("continuous recording closes one segment and opens the next at its keyframe")
{
  ClipBuilder builder;
  builder.configure({.preRollMs = 10000,
                     .postRollMs = 10000,
                     .segmentMs = 5000,
                     .preRollBytes = size_t{1024} * 1024,
                     .maxClipBytes = size_t{1024} * 1024});
  builder.setInit(initBytes());
  const int64_t epochMs = 1700000000000;
  CHECK(builder.request(CameraClipTrigger::Continuous, 1000, epochMs));
  pushChunk(builder, chunkOf(1), true, 2000);
  CHECK_FALSE(builder.takeReady().has_value());
  pushChunk(builder, chunkOf(2), false, 6000);
  CHECK_FALSE(builder.takeReady().has_value());
  pushChunk(builder, chunkOf(3), false, 7000);
  CHECK_FALSE(builder.takeReady().has_value());

  pushChunk(builder, chunkOf(4), true, 8000);
  const auto first = builder.takeReady();
  if (!first.has_value())
  {
    FAIL("the first segment never surfaced");
    return;
  }
  CHECK(first->trigger == CameraClipTrigger::Continuous);
  CHECK(first->body == initBytes() + chunkOf(1) + chunkOf(2) + chunkOf(3));
  CHECK(first->durationMs == 5000);
  CHECK(first->startedAtMs == epochMs + 1000);
  CHECK(builder.active());

  pushChunk(builder, chunkOf(5), false, 11500);
  CHECK_FALSE(builder.takeReady().has_value());
  pushChunk(builder, chunkOf(6), true, 13000);

  const auto second = builder.takeReady();
  if (!second.has_value())
  {
    FAIL("the second segment never surfaced");
    return;
  }
  CHECK(second->trigger == CameraClipTrigger::Continuous);
  CHECK(second->body == initBytes() + chunkOf(4) + chunkOf(5));
  CHECK(second->durationMs == 3500);
  CHECK(second->startedAtMs == epochMs + 7000);
  CHECK(builder.active());
}

TEST_CASE("the ring keeps only the pre-roll window and the clip stays under its cap")
{
  ClipBuilder builder;
  builder.configure({.preRollMs = 3000,
                     .postRollMs = 1000,
                     .segmentMs = 120000,
                     .preRollBytes = 64,
                     .maxClipBytes = 40});
  builder.setInit(initBytes());

  for (int64_t index = 0; index < 10; ++index)
    pushChunk(builder, chunkOf(static_cast<std::size_t>(index)), true,
               index * 1000);
  CHECK(builder.request(CameraClipTrigger::Event, 10000, 1700000000000));
  pushChunk(builder, chunkOf(100), true, 10500);

  const auto ready = builder.takeReady();
  if (!ready.has_value())
  {
    FAIL("the capped clip never surfaced");
    return;
  }
  CHECK(ready->body == initBytes() + chunkOf(7) + chunkOf(8) + chunkOf(9));
  CHECK(ready->durationMs == 2000);
  pushChunk(builder, chunkOf(101), false, 10600);
  CHECK_FALSE(builder.takeReady().has_value());
}

TEST_CASE("a clip without an init segment never surfaces")
{
  ClipBuilder builder;
  builder.configure(testConfig());

  pushChunk(builder, chunkOf(1), true, 1000);
  CHECK(builder.request(CameraClipTrigger::Event, 2000, 0));
  pushChunk(builder, chunkOf(2), true, 3000);
  pushChunk(builder, chunkOf(3), false, 12000);
  CHECK_FALSE(builder.takeReady().has_value());

  builder.reset();
  CHECK_FALSE(builder.active());
  CHECK_FALSE(builder.hasInit());
  CHECK_FALSE(builder.takeReady().has_value());
}

TEST_CASE("the recorder answers no request for a camera it does not record")
{
  ClipRecorder& recorder = ClipRecorder::instance();
  CHECK_FALSE(recorder.requestClip(
      {.cameraId = 4242, .trigger = CameraClipTrigger::Event, .atEpochMs = 0}));
  const ClipRecorderStatus status = recorder.status();
  CHECK(status.subscribed == 0);
  CHECK(status.active == 0);
}

namespace
{
std::vector<int64_t> chunkIndices(const std::string& body)
{
  std::vector<int64_t> indices;
  std::size_t at = body.find("chunk-");
  while (at != std::string::npos)
  {
    const std::size_t from = at + 6;
    const std::size_t to = body.find('-', from);
    indices.push_back(std::stoll(body.substr(from, to - from)));
    at = body.find("chunk-", to);
  }
  return indices;
}
}

TEST_CASE("every shipped continuous length closes on a keyframe and opens the next on it")
{
  for (const int64_t segmentMs : {int64_t{10000}, int64_t{30000}, int64_t{60000}, int64_t{120000}})
  {
    ClipBuilder builder;
    builder.configure({.preRollMs = 10000,
                       .postRollMs = 10000,
                       .segmentMs = segmentMs,
                       .preRollBytes = size_t{8} * 1024 * 1024,
                       .maxClipBytes = size_t{8} * 1024 * 1024});
    builder.setInit(initBytes());
    const int64_t base = 100000;
    const int64_t epochMs = 1700000000000;
    CHECK(builder.request(CameraClipTrigger::Continuous, base, epochMs));

    std::vector<std::vector<int64_t>> segments;
    std::vector<ReadyClip> readyClips;
    int64_t frame = 0;
    while (readyClips.size() < 3 && frame < 40000)
    {
      const int64_t atMs = base + frame * 40;
      const bool keyframe = frame % 50 == 0;
      pushChunk(builder, chunkOf(static_cast<std::size_t>(frame)), keyframe, atMs);
      if (const auto ready = builder.takeReady(); ready.has_value())
      {
        segments.push_back(chunkIndices(ready->body));
        readyClips.push_back(*ready);
      }
      ++frame;
    }
    REQUIRE(readyClips.size() == 3);
    for (const auto& indices : segments)
      CHECK_FALSE(indices.empty());
    for (std::size_t index = 1; index < segments.size(); ++index)
    {
      REQUIRE_FALSE(segments[index - 1].empty());
      const int64_t previousLast = segments[index - 1].back();
      const int64_t thisFirst = segments[index].front();
      CHECK(thisFirst == previousLast + 1);
    }
    for (const auto& clip : readyClips)
      CHECK(clip.trigger == CameraClipTrigger::Continuous);
    for (std::size_t index = 0; index < readyClips.size(); ++index)
    {
      const int64_t first = segments[index].front();
      const int64_t last = segments[index].back();
      CHECK(readyClips[index].startedAtMs == epochMs + first * 40);
      CHECK(readyClips[index].durationMs == (last - first) * 40);
      if (index > 0)
        CHECK(first * 40 >= segmentMs);
    }
    CHECK(readyClips.front().durationMs <= segmentMs);
  }
}

TEST_CASE("a motion clip still closes on the post-roll window, not on a keyframe")
{
  ClipBuilder builder;
  builder.configure(testConfig());
  builder.setInit(initBytes());
  CHECK(builder.request(CameraClipTrigger::Motion, 1000, 1700000000000));
  pushChunk(builder, chunkOf(1), true, 1000);
  pushChunk(builder, chunkOf(2), false, 5000);
  CHECK_FALSE(builder.takeReady().has_value());
  pushChunk(builder, chunkOf(3), false, 11000);
  const auto ready = builder.takeReady();
  REQUIRE(ready.has_value());
  CHECK(ready->trigger == CameraClipTrigger::Motion);
  CHECK(ready->durationMs == 10000);
}

TEST_CASE("the watchdog warns once, backs off, and honours the observed keyframe interval")
{
  using namespace record_watchdog;
  const Limits limits{};
  Slot slot;
  slot.lastFragmentMs = 1000;
  CHECK_FALSE(verdict(slot, 1000 + 5999, limits).stalled);
  const auto first = verdict(slot, 1000 + 6000, limits);
  CHECK(first.stalled);
  CHECK(first.warn);
  CHECK(first.retry);
  CHECK(first.backoffMs == 5000);
  CHECK(first.retryAtMs == 1000 + 6000 + 5000);

  Slot pending = slot;
  pending.stalled = true;
  pending.retryBackoffMs = first.backoffMs;
  pending.retryAtMs = first.retryAtMs;
  const auto waiting = verdict(pending, first.retryAtMs - 1, limits);
  CHECK(waiting.stalled);
  CHECK_FALSE(waiting.warn);
  CHECK_FALSE(waiting.retry);
  const auto second = verdict(pending, first.retryAtMs, limits);
  CHECK(second.retry);
  CHECK_FALSE(second.warn);
  CHECK(second.backoffMs == 10000);

  Slot capped = pending;
  capped.retryBackoffMs = 60000;
  capped.retryAtMs = 0;
  CHECK(verdict(capped, 1000000, limits).backoffMs == 60000);

  Slot wide = slot;
  wide.keyframeIntervalMs = 10000;
  CHECK_FALSE(verdict(wide, 1000 + 19999, limits).stalled);
  CHECK(verdict(wide, 1000 + 20000, limits).stalled);

  Slot never = slot;
  never.lastFragmentMs = 0;
  CHECK(verdict(never, 1000000, limits).stalled);
}

TEST_CASE("the keyframe interval is the observed average, not a guess")
{
  using namespace record_watchdog;
  Interval interval;
  observeKeyframe(interval, 1000);
  CHECK(interval.intervalMs == 0);
  CHECK(interval.lastKeyframeMs == 1000);
  observeKeyframe(interval, 3000);
  CHECK(interval.intervalMs == 2000);
  observeKeyframe(interval, 7000);
  CHECK(interval.intervalMs == 2500);
  observeKeyframe(interval, 7000);
  CHECK(interval.intervalMs == 2500);
  observeKeyframe(interval, 5000);
  CHECK(interval.intervalMs == 2500);
}

TEST_CASE("the disk floor reads a real filesystem and answers nothing for a missing path")
{
  CHECK(disk_space::freeBytesOf("/") > 0);
  CHECK(disk_space::freeBytesOf("/tmp") > 0);
  CHECK(disk_space::freeBytesOf("/definitely/not/a/path") == -1);
}
