#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct TapoResampleInput
{
  std::vector<int16_t> samples;
  int sourceRate{44100};
  int targetRate{8000};
};

struct TapoWavAudio
{
  bool ok{false};
  std::string error;
  std::vector<int16_t> samples;
  int sampleRate{0};
  int channels{0};
};

namespace tapo_audio
{

std::vector<int16_t> resample(const TapoResampleInput& input);
std::vector<int16_t> downmixToMono(const std::vector<int16_t>& samples,
                                   int channels);
std::vector<uint8_t> encodeALaw(const std::vector<int16_t>& samples);
std::vector<int16_t> decodeALaw(const std::vector<uint8_t>& samples);
std::vector<int16_t> decodeULaw(const std::vector<uint8_t>& samples);
TapoWavAudio readWav(const std::string& path);

} // namespace tapo_audio
