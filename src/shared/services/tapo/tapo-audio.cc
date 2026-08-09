#include "tapo-audio.hxx"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <shared/wrapper/audio/audio-resampler.hxx>

namespace
{

uint8_t alawEncodeSample(int16_t sample)
{
  static const int16_t kSegmentEnd[8] = {0xFF,  0x1FF,  0x3FF,  0x7FF,
                                         0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};
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

uint32_t readLe32(const char* data)
{
  uint32_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

uint16_t readLe16(const char* data)
{
  uint16_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

} // namespace

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
  resampler.process(input.samples.data(), input.samples.size(), out);
  return out;
}

std::vector<int16_t> downmixToMono(const std::vector<int16_t>& samples,
                                   int channels)
{
  if (channels <= 1)
    return samples;
  std::vector<int16_t> out;
  out.reserve(samples.size() / static_cast<size_t>(channels));
  for (size_t i = 0; i + static_cast<size_t>(channels) <= samples.size();
       i += static_cast<size_t>(channels)) {
    int32_t sum = 0;
    for (int channel = 0; channel < channels; ++channel)
      sum += samples[i + static_cast<size_t>(channel)];
    out.push_back(static_cast<int16_t>(sum / channels));
  }
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

TapoWavAudio readWav(const std::string& path)
{
  TapoWavAudio audio;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    audio.error = "cannot open " + path;
    return audio;
  }

  std::string data((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  if (data.size() < 44 || data.compare(0, 4, "RIFF") != 0 ||
      data.compare(8, 4, "WAVE") != 0) {
    audio.error = "not a RIFF/WAVE file";
    return audio;
  }

  size_t cursor = 12;
  int bitsPerSample = 0;
  size_t dataOffset = 0;
  size_t dataSize = 0;
  while (cursor + 8 <= data.size()) {
    const std::string id = data.substr(cursor, 4);
    const uint32_t size = readLe32(data.data() + cursor + 4);
    const size_t body = cursor + 8;
    if (id == "fmt " && body + 16 <= data.size()) {
      audio.channels = readLe16(data.data() + body + 2);
      audio.sampleRate = static_cast<int>(readLe32(data.data() + body + 4));
      bitsPerSample = readLe16(data.data() + body + 14);
    }
    else if (id == "data") {
      dataOffset = body;
      dataSize = std::min(static_cast<size_t>(size), data.size() - body);
    }
    cursor = body + size + (size % 2);
  }

  if (bitsPerSample != 16 || dataOffset == 0 || audio.channels <= 0) {
    audio.error = "only 16-bit PCM WAV is supported";
    return audio;
  }

  audio.samples.resize(dataSize / sizeof(int16_t));
  std::memcpy(audio.samples.data(), data.data() + dataOffset,
              audio.samples.size() * sizeof(int16_t));
  audio.ok = true;
  return audio;
}

} // namespace tapo_audio
