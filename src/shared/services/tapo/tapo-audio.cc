#include "tapo-audio.hxx"

#include <algorithm>
#include <array>
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

namespace
{

constexpr int16_t kALawDecodeTable[256] = {
    -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736, -7552, -7296, -8064, -7808,
    -6528, -6272, -7040, -6784, -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368,
    -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392, -22016, -20992, -24064, -23040,
    -17920, -16896, -19968, -18944, -30208, -29184, -32256, -31232, -26112, -25088, -28160, -27136,
    -11008, -10496, -12032, -11520, -8960, -8448, -9984, -9472, -15104, -14592, -16128, -15616,
    -13056, -12544, -14080, -13568, -344, -328, -376, -360, -280, -264, -312, -296,
    -472, -456, -504, -488, -408, -392, -440, -424, -88, -72, -120, -104,
    -24, -8, -56, -40, -216, -200, -248, -232, -152, -136, -184, -168,
    -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184, -1888, -1824, -2016, -1952,
    -1632, -1568, -1760, -1696, -688, -656, -752, -720, -560, -528, -624, -592,
    -944, -912, -1008, -976, -816, -784, -880, -848, 5504, 5248, 6016, 5760,
    4480, 4224, 4992, 4736, 7552, 7296, 8064, 7808, 6528, 6272, 7040, 6784,
    2752, 2624, 3008, 2880, 2240, 2112, 2496, 2368, 3776, 3648, 4032, 3904,
    3264, 3136, 3520, 3392, 22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944,
    30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136, 11008, 10496, 12032, 11520,
    8960, 8448, 9984, 9472, 15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568,
    344, 328, 376, 360, 280, 264, 312, 296, 472, 456, 504, 488,
    408, 392, 440, 424, 88, 72, 120, 104, 24, 8, 56, 40,
    216, 200, 248, 232, 152, 136, 184, 168, 1376, 1312, 1504, 1440,
    1120, 1056, 1248, 1184, 1888, 1824, 2016, 1952, 1632, 1568, 1760, 1696,
    688, 656, 752, 720, 560, 528, 624, 592, 944, 912, 1008, 976,
    816, 784, 880, 848,
};

constexpr int16_t kULawDecodeTable[256] = {
    -32124, -31100, -30076, -29052, -28028, -27004, -25980, -24956, -23932, -22908, -21884, -20860,
    -19836, -18812, -17788, -16764, -15996, -15484, -14972, -14460, -13948, -13436, -12924, -12412,
    -11900, -11388, -10876, -10364, -9852, -9340, -8828, -8316, -7932, -7676, -7420, -7164,
    -6908, -6652, -6396, -6140, -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
    -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004, -2876, -2748, -2620, -2492,
    -2364, -2236, -2108, -1980, -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
    -1372, -1308, -1244, -1180, -1116, -1052, -988, -924, -876, -844, -812, -780,
    -748, -716, -684, -652, -620, -588, -556, -524, -492, -460, -428, -396,
    -372, -356, -340, -324, -308, -292, -276, -260, -244, -228, -212, -196,
    -180, -164, -148, -132, -120, -112, -104, -96, -88, -80, -72, -64,
    -56, -48, -40, -32, -24, -16, -8, 0, 32124, 31100, 30076, 29052,
    28028, 27004, 25980, 24956, 23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
    15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412, 11900, 11388, 10876, 10364,
    9852, 9340, 8828, 8316, 7932, 7676, 7420, 7164, 6908, 6652, 6396, 6140,
    5884, 5628, 5372, 5116, 4860, 4604, 4348, 4092, 3900, 3772, 3644, 3516,
    3388, 3260, 3132, 3004, 2876, 2748, 2620, 2492, 2364, 2236, 2108, 1980,
    1884, 1820, 1756, 1692, 1628, 1564, 1500, 1436, 1372, 1308, 1244, 1180,
    1116, 1052, 988, 924, 876, 844, 812, 780, 748, 716, 684, 652,
    620, 588, 556, 524, 492, 460, 428, 396, 372, 356, 340, 324,
    308, 292, 276, 260, 244, 228, 212, 196, 180, 164, 148, 132,
     120, 112, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32,
     24, 16, 8, 0,
};

} // namespace tapo_audio

std::vector<int16_t> decodeALaw(const std::vector<uint8_t>& samples)
{
  std::vector<int16_t> out;
  out.reserve(samples.size());
  for (const uint8_t sample : samples)
    out.push_back(kALawDecodeTable[sample]);
  return out;
}

std::vector<int16_t> decodeULaw(const std::vector<uint8_t>& samples)
{
  std::vector<int16_t> out;
  out.reserve(samples.size());
  for (const uint8_t sample : samples)
    out.push_back(kULawDecodeTable[sample]);
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
