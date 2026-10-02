#include "tapo-audio.hxx"

#include <array>
#include <audio/audio-resampler.hxx>

namespace
{

uint8_t alawEncodeSample(int16_t sample)
{
  static constexpr std::array<int16_t, 8> kSegmentEnd = {
      0xFF,  0x1FF,  0x3FF,  0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};
  int sign = (sample & 0x8000) >> 8;
  if (sign != 0)
    sample = static_cast<int16_t>(-sample);
  if (sample > 32635)
    sample = 32635;

  int segment = 0;
  while (segment < 8 && sample > kSegmentEnd[segment])
    ++segment;

  uint8_t encoded = 0;
  if (segment >= 8)
    encoded = static_cast<uint8_t>(0x7F ^ 0x55);
  else if (segment < 2)
    encoded = static_cast<uint8_t>(((sample >> 4) & 0x0F) | (segment << 4));
  else
    encoded = static_cast<uint8_t>(((sample >> (segment + 3)) & 0x0F) |
                                   (segment << 4));

  return static_cast<uint8_t>((encoded ^ 0x55) | sign);
}

}

namespace tapo_audio
{

std::vector<int16_t> resample(const TapoResampleInput& input)
{
  if (input.samples.empty() || input.sourceRate <= 0 || input.targetRate <= 0)
    return {};
  if (input.sourceRate == input.targetRate)
    return input.samples;
  AudioResampler resampler(
      {.sourceRate = input.sourceRate, .targetRate = input.targetRate});
  std::vector<int16_t> out;
  out = resampler.process(input.samples.data(), input.samples.size());
  return out;
}

std::vector<uint8_t> encodeALaw(const std::vector<int16_t>& samples)
{
  std::vector<uint8_t> out;
  out.reserve(samples.size());
  for (const int16_t sample : samples)
    out.push_back(alawEncodeSample(sample));
  return out;
}

}
