#include "near-field-gate.hxx"

#include <algorithm>
#include <cmath>

float rmsDbOf(std::span<const float> samples)
{
  if (samples.empty())
    return kSilentDb;
  double sum = 0.0;
  for (const float sample : samples)
    sum += static_cast<double>(sample) * static_cast<double>(sample);
  const double rms = std::sqrt(sum / static_cast<double>(samples.size()));
  if (rms <= 1e-6)
    return kSilentDb;
  return static_cast<float>(20.0 * std::log10(rms));
}

NoiseFloor::NoiseFloor(const NearFieldConfig& cfg)
    : ring_(static_cast<std::size_t>(std::max(1, cfg.floorWindows)), kSilentDb),
      percentile_(std::clamp(cfg.floorPercentile, 0.0F, 1.0F))
{
}

void NoiseFloor::add(float rmsDb)
{
  ring_[next_] = rmsDb;
  next_ = (next_ + 1) % ring_.size();
  count_ = std::min(count_ + 1, ring_.size());
}

void NoiseFloor::reset()
{
  std::ranges::fill(ring_, kSilentDb);
  next_ = 0;
  count_ = 0;
}

float NoiseFloor::db() const
{
  if (count_ < kMinFloorSamples)
    return kDefaultFloorDb;
  std::vector<float> seen(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(count_));
  const auto index = static_cast<std::ptrdiff_t>(percentile_ * static_cast<float>(seen.size() - 1));
  std::nth_element(seen.begin(), seen.begin() + index, seen.end());
  return seen[static_cast<std::size_t>(index)];
}

NearFieldVerdict evaluateNearField(std::span<const GateWindow> turn, float floorDb,
                                   const NearFieldConfig& cfg)
{
  NearFieldVerdict verdict;
  verdict.floorDb = floorDb;

  double levelSum = 0.0;
  int levelCount = 0;
  int run = 0;
  int longest = 0;
  for (const GateWindow& window : turn) {
    if (window.prob >= cfg.speechProb) {
      levelSum += window.rmsDb;
      ++levelCount;
      ++run;
      longest = std::max(longest, run);
    }
    else {
      run = 0;
    }
  }
  verdict.heldMs = longest * kGateWindowMs;
  if (levelCount == 0)
    return verdict;
  verdict.levelDb = static_cast<float>(levelSum / levelCount);
  verdict.marginDb = verdict.levelDb - floorDb;
  verdict.passed = verdict.marginDb >= cfg.marginDb && verdict.heldMs >= cfg.heldMs;
  return verdict;
}
