#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <feature/rtc/playout-buffering.hxx>

#include <vector>

namespace
{
constexpr size_t kPreRollSamples = 16000 * 400 / 1000;
constexpr int kTickMs = 10;
constexpr int kChunkMs = 20;
constexpr size_t kTickSamples = 160;
constexpr size_t kChunkSamples = 320;

int underrunTicksFor(size_t preRollSamples)
{
  PlayoutBuffering buffering(preRollSamples, kPlayoutTailFlushAfter);
  std::vector<int> arrival;
  arrival.reserve(60);
  for (int k = 0; k < 60; ++k)
    arrival.push_back(kChunkMs * k + (k % 5 == 0 ? 60 : 0));
  size_t buffered = 0;
  size_t next = 0;
  int underruns = 0;
  for (int t = 0; t < 1400; t += kTickMs) {
    while (next < arrival.size() && arrival[next] <= t) {
      buffered += kChunkSamples;
      ++next;
    }
    if (buffered >= kTickSamples && buffering.startsPlayout(buffered))
      buffered -= kTickSamples;
    else if (buffering.preRolled())
      ++underruns;
  }
  return underruns;
}
}

TEST_CASE("nothing plays until the pre-roll is buffered, then it keeps playing until drained")
{
  PlayoutBuffering buffering(kPreRollSamples, kPlayoutTailFlushAfter);
  CHECK_FALSE(buffering.startsPlayout(160));
  CHECK_FALSE(buffering.startsPlayout(kPreRollSamples - 1));
  CHECK(buffering.startsPlayout(kPreRollSamples));
  CHECK(buffering.startsPlayout(160));
  buffering.drained();
  CHECK_FALSE(buffering.preRolled());
  CHECK_FALSE(buffering.startsPlayout(160));
}

TEST_CASE("a partial frame waits for more audio until the tail flush window has passed")
{
  PlayoutBuffering buffering(kPreRollSamples, kPlayoutTailFlushAfter);
  CHECK_FALSE(buffering.tailDue(std::chrono::milliseconds(199)));
  CHECK(buffering.tailDue(std::chrono::milliseconds(201)));
}

TEST_CASE("the pre-roll absorbs the stalls of a stream that runs at real time")
{
  CHECK(underrunTicksFor(0) > 0);
  CHECK(underrunTicksFor(kPreRollSamples) == 0);
}

TEST_CASE("a tail flush takes at most one frame, so a burst held below the pre-roll never overruns the frame buffer")
{
  constexpr size_t kFrame = 480;
  CHECK(tailPlayoutSamples(14400, kFrame) == kFrame);
  CHECK(tailPlayoutSamples(19200, kFrame) == kFrame);
  CHECK(tailPlayoutSamples(kFrame, kFrame) == kFrame);
  CHECK(tailPlayoutSamples(100, kFrame) == 100);
  CHECK(tailPlayoutSamples(0, kFrame) == 0);
}

TEST_CASE("a burst held below the pre-roll drains through the real tail flush, one frame at a time, never past the frame")
{
  constexpr size_t kFrame = 480;
  constexpr size_t kBurst = 14400;
  BasicSampleRing<int16_t> ring(32000);
  std::vector<int16_t> burst(kBurst);
  for (size_t i = 0; i < kBurst; ++i)
    burst[i] = static_cast<int16_t>(i % 1000);
  ring.push(burst.data(), burst.size());

  std::vector<int16_t> frame(kFrame);
  size_t drained = 0;
  size_t ticks = 0;
  while (ring.size() > 0 && ticks < 200) {
    const size_t taken = takeTailFrame(ring, {frame.data(), frame.size()});
    CHECK(taken <= kFrame);
    drained += taken;
    ++ticks;
  }
  CHECK(drained == kBurst);
  CHECK(ring.size() == 0);
  CHECK(ticks == kBurst / kFrame);
}
