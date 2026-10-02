#pragma once

#include <cstdint>
#include <vector>

struct TapoResampleInput
{
  std::vector<int16_t> samples;
  int sourceRate{44100};
  int targetRate{8000};
};

namespace tapo_audio
{

std::vector<int16_t> resample(const TapoResampleInput& input);
std::vector<uint8_t> encodeALaw(const std::vector<int16_t>& samples);

}
