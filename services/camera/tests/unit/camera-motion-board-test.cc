#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/camera-motion/camera-motion-board.hxx>

#include <cstdint>

namespace
{
constexpr int64_t kStart = 1791910800000;
constexpr int64_t kSecond = 1000;
constexpr int64_t kMinute = 60 * kSecond;
constexpr int64_t kCamera = 7;

struct Motion
{
  CameraMotionBoard board;
  int64_t nowMs{kStart};

  void advance(int64_t ms) { nowMs += ms; }

  void onset(int64_t cameraId = kCamera) { board.report({.cameraId = cameraId, .moving = true, .atMs = nowMs}); }

  void end(int64_t cameraId = kCamera) { board.report({.cameraId = cameraId, .moving = false, .atMs = nowMs}); }

  [[nodiscard]] bool raises(int64_t cameraId = kCamera) { return board.raisesCadence(cameraId, nowMs); }
};
}

TEST_CASE("an onset raises the cadence at once, and an end keeps it for the hold only")
{
  Motion motion;
  CHECK_FALSE(motion.raises());
  motion.onset();
  CHECK(motion.raises());
  motion.advance(30 * kSecond);
  CHECK(motion.raises());
  motion.end();
  motion.advance(CameraMotionBoard::kHoldMs - 1);
  CHECK(motion.raises());
  motion.advance(1);
  CHECK_FALSE(motion.raises());
  CHECK(motion.board.mutedEpisodes() == 0);
}

TEST_CASE("an end with no onset starts no hold, and each camera keeps its own motion")
{
  Motion motion;
  motion.end();
  CHECK_FALSE(motion.raises());
  motion.onset(8);
  CHECK(motion.raises(8));
  CHECK_FALSE(motion.raises());
  motion.onset();
  motion.board.forget(kCamera);
  CHECK_FALSE(motion.raises());
  CHECK(motion.raises(8));
}

TEST_CASE("motion held on for more than ten minutes stops raising the cadence until the camera goes quiet")
{
  Motion motion;
  motion.onset();
  motion.advance(CameraMotionBoard::kStuckMs);
  CHECK(motion.raises());
  motion.advance(1);
  CHECK_FALSE(motion.raises());
  CHECK(motion.board.mutedEpisodes() == 1);
  motion.onset();
  motion.advance(20 * kMinute);
  CHECK_FALSE(motion.raises());
  CHECK(motion.board.mutedEpisodes() == 1);

  motion.end();
  motion.advance(2 * kSecond);
  CHECK_FALSE(motion.raises());
  motion.onset();
  CHECK_FALSE(motion.raises());
  motion.end();
  motion.advance(CameraMotionBoard::kHoldMs);
  CHECK_FALSE(motion.raises());

  motion.onset();
  CHECK(motion.raises());
  CHECK(motion.board.mutedEpisodes() == 1);
}

TEST_CASE("a camera that keeps re-firing within the hold is one episode, muted after ten minutes")
{
  Motion motion;
  const int64_t cycle = 4 * kSecond;
  int64_t elapsed = 0;
  while (elapsed + cycle <= CameraMotionBoard::kStuckMs) {
    motion.onset();
    motion.advance(cycle / 2);
    motion.end();
    motion.advance(cycle / 2);
    elapsed += cycle;
    REQUIRE(motion.raises());
  }
  motion.onset();
  motion.advance(cycle / 2);
  motion.end();
  motion.advance(cycle / 2);
  CHECK_FALSE(motion.raises());
  CHECK(motion.board.mutedEpisodes() == 1);
  motion.onset();
  motion.advance(cycle / 2);
  motion.end();
  CHECK_FALSE(motion.raises());
  CHECK(motion.board.mutedEpisodes() == 1);

  motion.advance(CameraMotionBoard::kHoldMs);
  CHECK_FALSE(motion.raises());
  motion.onset();
  CHECK(motion.raises());
}

TEST_CASE("an onset after a full hold of quiet starts a fresh episode with a fresh ten minutes")
{
  Motion motion;
  motion.onset();
  motion.advance(9 * kMinute);
  motion.end();
  motion.advance(CameraMotionBoard::kHoldMs);
  motion.onset();
  motion.advance(9 * kMinute);
  CHECK(motion.raises());
  CHECK(motion.board.mutedEpisodes() == 0);
}
