#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <span>

struct CameraLevelConfig
{
  float floorRiseDbPerSecond{0.5F};
  float floorFallDbPerSecond{10.0F};
  float initialFloorDb{-60.0F};
  std::chrono::milliseconds window{2000};
  int sampleRate{16000};
};

struct LevelFrame
{
  float db{-120.0F};
  bool speech{false};
};

[[nodiscard]] float rmsDbOf(std::span<const float> samples);

class CameraLevel
{
public:
  explicit CameraLevel(CameraLevelConfig config);

  void observe(std::span<const float> frame, bool speech);

  [[nodiscard]] float floorDb() const { return floorDb_; }
  [[nodiscard]] float speechSnrDb() const;
  [[nodiscard]] float lastDb() const { return lastDb_; }

private:
  CameraLevelConfig config_;
  float floorDb_;
  float lastDb_{-120.0F};
  std::deque<LevelFrame> window_;
  size_t windowFrames_{0};
};
