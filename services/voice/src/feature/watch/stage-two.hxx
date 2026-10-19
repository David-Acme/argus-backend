#pragma once

#include <feature/voice/voice-engine-seam.hxx>
#include <feature/watch/wake-words.hxx>

#include <string>
#include <vector>

struct StageTwoInput
{
  const std::vector<float>& samples;
  std::string language;
  int maxDistance{2};
};

struct StageTwoResult
{
  bool accepted{false};
  bool transcribed{false};
  WakeMatch match;
  std::string transcript;
  float confidence{0.0F};
};

[[nodiscard]] StageTwoResult judgeWake(const std::string& transcript, int maxDistance);

class StageTwo
{
public:
  explicit StageTwo(IVoiceStt& stt) : stt_(stt) {}

  [[nodiscard]] StageTwoResult confirm(const StageTwoInput& input) const;

private:
  IVoiceStt& stt_;
};
