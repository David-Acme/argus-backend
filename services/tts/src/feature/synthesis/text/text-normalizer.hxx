#pragma once

#include <cstdint>
#include <string>
#include <string_view>

enum class SpeechLanguage : std::uint8_t
{
  Spanish,
  English,
  Other
};

[[nodiscard]] SpeechLanguage speechLanguage(std::string_view code);

[[nodiscard]] std::string normalizeSpeechText(std::string_view text, SpeechLanguage language);
