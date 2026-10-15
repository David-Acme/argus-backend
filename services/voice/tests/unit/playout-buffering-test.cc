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
