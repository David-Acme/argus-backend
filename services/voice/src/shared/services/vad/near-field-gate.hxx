#pragma once

#include <cstddef>
#include <span>
#include <vector>

struct NearFieldConfig
{
  float marginDb{30.0F};
  float speechProb{0.5F};
  int heldMs{160};
  int floorWindows{300};
  float floorPercentile{0.2F};
};

struct GateWindow
{
  float rmsDb{-120.0F};
  float prob{0.0F};
};

struct NearFieldVerdict
{
  float levelDb{-120.0F};
  float floorDb{-60.0F};
  float marginDb{0.0F};
  int heldMs{0};
  bool passed{false};
};

inline constexpr int kGateWindowMs = 32;
inline constexpr float kSilentDb = -120.0F;
inline constexpr float kDefaultFloorDb = -60.0F;
inline constexpr std::size_t kMinFloorSamples = 10;

[[nodiscard]] float rmsDbOf(std::span<const float> samples);

class NoiseFloor
{
public:
  explicit NoiseFloor(const NearFieldConfig& cfg);

  void add(float rmsDb);
  void reset();

  [[nodiscard]] float db() const;

private:
  std::vector<float> ring_;
  std::size_t next_{0};
  std::size_t count_{0};
  float percentile_{0.2F};
};

[[nodiscard]] NearFieldVerdict evaluateNearField(std::span<const GateWindow> turn, float floorDb,
                                                 const NearFieldConfig& cfg);
