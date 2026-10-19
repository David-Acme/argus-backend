#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

struct SpeakerPaddingInput
{
  int rate{8000};
  int64_t earconMs{300};
  int64_t leadSilenceMs{700};
  bool earcon{true};
};

[[nodiscard]] std::vector<int16_t> silenceOf(int rate, int64_t ms);

[[nodiscard]] std::vector<int16_t> earconTone(int rate, int64_t ms);

[[nodiscard]] std::vector<int16_t> speakerPrologue(const SpeakerPaddingInput& input);

[[nodiscard]] std::vector<int16_t> pcmSamplesOf(std::string_view bytes);
