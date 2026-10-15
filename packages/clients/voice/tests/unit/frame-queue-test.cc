#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <voice/frame-queue.hxx>

namespace
{
namespace v1 = argus::voice::v1;

v1::ClientFrame muteFrame(bool muted)
{
  v1::ClientFrame frame;
  frame.mutable_mute()->set_muted(muted);
  return frame;
}
}

TEST_CASE("a pushed frame is handed to the writer once and the next one waits for it")
{
  FrameQueue queue;
  CHECK(queue.push(muteFrame(true)));
  CHECK(queue.push(muteFrame(false)));
  auto* first = queue.nextWrite();
  REQUIRE(first != nullptr);
  CHECK(first->mute().muted());
  CHECK(queue.nextWrite() == nullptr);
  queue.completeWrite(true);
  auto* second = queue.nextWrite();
  REQUIRE(second != nullptr);
  CHECK_FALSE(second->mute().muted());
}

TEST_CASE("a failed write drops the frames still queued behind it")
{
  FrameQueue queue;
  CHECK(queue.push(muteFrame(true)));
  CHECK(queue.push(muteFrame(false)));
  REQUIRE(queue.nextWrite() != nullptr);
  queue.completeWrite(false);
  CHECK(queue.size() == 0);
  CHECK(queue.nextWrite() == nullptr);
}

TEST_CASE("the queue refuses frames beyond its capacity")
{
  FrameQueue queue;
  for (size_t i = 0; i < kMaxPendingWrites; ++i)
    REQUIRE(queue.push(muteFrame(true)));
  CHECK_FALSE(queue.push(muteFrame(true)));
  CHECK(queue.size() == kMaxPendingWrites);
}

TEST_CASE("cancel while a write is in flight keeps that frame and drops the rest")
{
  FrameQueue queue;
  CHECK(queue.push(muteFrame(true)));
  CHECK(queue.push(muteFrame(false)));
  CHECK(queue.push(muteFrame(false)));
  auto* inFlight = queue.nextWrite();
  REQUIRE(inFlight != nullptr);
  queue.cancel();
  CHECK(queue.cancelled());
  CHECK(queue.size() == 1);
  CHECK(inFlight->mute().muted());
  CHECK_FALSE(queue.push(muteFrame(true)));
  queue.completeWrite(true);
  CHECK(queue.size() == 0);
  CHECK(queue.nextWrite() == nullptr);
}

TEST_CASE("cancel with nothing in flight empties the queue at once")
{
  FrameQueue queue;
  CHECK(queue.push(muteFrame(true)));
  CHECK(queue.push(muteFrame(false)));
  queue.cancel();
  CHECK(queue.size() == 0);
  CHECK(queue.nextWrite() == nullptr);
  CHECK_FALSE(queue.push(muteFrame(true)));
}
