#pragma once

#include <voice/voice-lang.hxx>

#include <cstdint>
#include <string_view>

[[nodiscard]] bool carriesWords(std::string_view transcript);

class UnheardRotation
{
public:
  explicit UnheardRotation(uint64_t seed) : seed_(seed) {}

  [[nodiscard]] std::string_view next(VoiceLang lang);

private:
  uint64_t seed_{0};
  uint64_t spoken_{0};
};
