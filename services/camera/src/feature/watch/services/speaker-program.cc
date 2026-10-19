#include "speaker-program.hxx"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace
{
constexpr double kFirstToneHz = 660.0;
constexpr double kSecondToneHz = 990.0;
constexpr double kPeak = 9000.0;
constexpr double kFadeShare = 0.15;

size_t samplesFor(int rate, int64_t ms)
{
  if (rate <= 0 || ms <= 0)
    return 0;
  return static_cast<size_t>(static_cast<int64_t>(rate) * ms / 1000);
}
}

std::vector<int16_t> silenceOf(int rate, int64_t ms)
{
  return std::vector<int16_t>(samplesFor(rate, ms), 0);
}

std::vector<int16_t> earconTone(int rate, int64_t ms)
{
  const size_t total = samplesFor(rate, ms);
  std::vector<int16_t> tone;
  tone.reserve(total);
  if (total == 0)
    return tone;
  const size_t half = total / 2;
  const auto fade = std::max<size_t>(1, static_cast<size_t>(static_cast<double>(half) * kFadeShare));
  double phase = 0.0;
  for (size_t index = 0; index < total; ++index) {
    const double frequency = index < half ? kFirstToneHz : kSecondToneHz;
    phase += 2.0 * std::numbers::pi * frequency / rate;
    const size_t local = index < half ? index : index - half;
    const size_t span = index < half ? half : total - half;
    const double rise = std::min(1.0, static_cast<double>(local) / static_cast<double>(fade));
    const double fall = std::min(1.0, static_cast<double>(span - local) / static_cast<double>(fade));
    const double envelope = std::min(rise, fall);
    tone.push_back(static_cast<int16_t>(std::sin(phase) * kPeak * envelope));
  }
  return tone;
}

std::vector<int16_t> speakerPrologue(const SpeakerPaddingInput& input)
{
  std::vector<int16_t> out;
  if (input.earcon) {
    out = earconTone(input.rate, input.earconMs);
  }
  else {
    out = silenceOf(input.rate, input.earconMs);
  }
  const std::vector<int16_t> lead = silenceOf(input.rate, input.leadSilenceMs);
  out.insert(out.end(), lead.begin(), lead.end());
  return out;
}

std::vector<int16_t> pcmSamplesOf(std::string_view bytes)
{
  std::vector<int16_t> samples(bytes.size() / 2);
  for (size_t index = 0; index < samples.size(); ++index) {
    const auto low = static_cast<uint16_t>(static_cast<unsigned char>(bytes[2 * index]));
    const auto high = static_cast<uint16_t>(static_cast<unsigned char>(bytes[(2 * index) + 1]));
    samples[index] = static_cast<int16_t>(static_cast<uint16_t>(low | static_cast<uint16_t>(high << 8U)));
  }
  return samples;
}
