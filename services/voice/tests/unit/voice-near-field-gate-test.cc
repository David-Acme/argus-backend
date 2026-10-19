#include <doctest/doctest.h>

#include <shared/services/vad/near-field-gate.hxx>

#include <array>
#include <cmath>
#include <vector>

namespace
{

std::vector<GateWindow> windows(int count, float rmsDb, float prob)
{
  return std::vector<GateWindow>(static_cast<size_t>(count), GateWindow{.rmsDb = rmsDb, .prob = prob});
}

std::vector<float> constantFrame(float amplitude)
{
  return std::vector<float>(512, amplitude);
}

}

TEST_CASE("the level of a frame is its RMS in dBFS, silence is the floor")
{
  CHECK(rmsDbOf(constantFrame(1.0F)) == doctest::Approx(0.0F).epsilon(0.01));
  CHECK(rmsDbOf(constantFrame(0.1F)) == doctest::Approx(-20.0F).epsilon(0.01));
  CHECK(rmsDbOf(constantFrame(0.0F)) == kSilentDb);
  CHECK(rmsDbOf(std::span<const float>{}) == kSilentDb);
}

TEST_CASE("far and quiet speech does not open a turn: its margin over the floor is too small")
{
  const NearFieldConfig cfg;
  const auto turn = windows(12, -52.0F, 0.9F);
  const NearFieldVerdict verdict = evaluateNearField(turn, -55.0F, cfg);
  CHECK(verdict.marginDb == doctest::Approx(3.0F).epsilon(0.01));
  CHECK_FALSE(verdict.passed);
}

TEST_CASE("near and loud speech held long enough opens the turn")
{
  const NearFieldConfig cfg;
  const auto turn = windows(10, -22.0F, 0.9F);
  const NearFieldVerdict verdict = evaluateNearField(turn, -58.0F, cfg);
  CHECK(verdict.marginDb == doctest::Approx(36.0F).epsilon(0.01));
  CHECK(verdict.heldMs == 10 * kGateWindowMs);
  CHECK(verdict.passed);
}

TEST_CASE("a loud noise burst too short to be speech does not open a turn")
{
  const NearFieldConfig cfg;
  const auto burst = windows(3, -10.0F, 0.95F);
  const NearFieldVerdict verdict = evaluateNearField(burst, -60.0F, cfg);
  CHECK(verdict.marginDb > cfg.marginDb);
  CHECK(verdict.heldMs < cfg.heldMs);
  CHECK_FALSE(verdict.passed);
}

TEST_CASE("held probability that is not loud enough over the floor does not open a turn")
{
  const NearFieldConfig cfg;
  const auto held = windows(20, -40.0F, 0.9F);
  const NearFieldVerdict verdict = evaluateNearField(held, -50.0F, cfg);
  CHECK(verdict.heldMs >= cfg.heldMs);
  CHECK(verdict.marginDb < cfg.marginDb);
  CHECK_FALSE(verdict.passed);
}

TEST_CASE("a turn without any window above the speech probability never passes")
{
  const NearFieldConfig cfg;
  const auto silent = windows(20, -20.0F, 0.1F);
  const NearFieldVerdict verdict = evaluateNearField(silent, -60.0F, cfg);
  CHECK(verdict.heldMs == 0);
  CHECK_FALSE(verdict.passed);
}

TEST_CASE("the noise floor is the low percentile of the recent quiet windows, with a default until it has samples")
{
  NearFieldConfig cfg;
  cfg.floorWindows = 50;
  cfg.floorPercentile = 0.2F;
  NoiseFloor floor(cfg);
  CHECK(floor.db() == kDefaultFloorDb);
  for (int i = 0; i < 9; ++i)
    floor.add(-40.0F);
  CHECK(floor.db() == kDefaultFloorDb);
  for (int i = 0; i < 20; ++i)
    floor.add(-60.0F + static_cast<float>(i));
  CHECK(floor.db() == doctest::Approx(-55.0F).epsilon(0.01));
  floor.reset();
  CHECK(floor.db() == kDefaultFloorDb);
}

TEST_CASE("the floor forgets windows that left its ring")
{
  NearFieldConfig cfg;
  cfg.floorWindows = 12;
  cfg.floorPercentile = 0.0F;
  NoiseFloor floor(cfg);
  for (int i = 0; i < 12; ++i)
    floor.add(-70.0F);
  for (int i = 0; i < 12; ++i)
    floor.add(-45.0F);
  CHECK(floor.db() == doctest::Approx(-45.0F).epsilon(0.01));
}
