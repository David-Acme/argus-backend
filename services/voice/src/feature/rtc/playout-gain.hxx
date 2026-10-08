#pragma once

#include <cstdint>
#include <span>

struct PlayoutGainSteps
{
  float duckGain{0.3F};
  int duckFrames{12};
  int releaseFrames{30};
};

class PlayoutGain
{
public:
  explicit PlayoutGain(PlayoutGainSteps steps = {});

  void duck(bool ducked);
  void apply(std::span<int16_t> frame);
  void fadeOut(std::span<int16_t> tail);

  [[nodiscard]] float current() const { return current_; }
  [[nodiscard]] float target() const { return target_; }

private:
  PlayoutGainSteps steps_;
  float current_{1.0F};
  float target_{1.0F};
};
