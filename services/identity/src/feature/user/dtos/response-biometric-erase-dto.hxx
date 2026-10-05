#pragma once

#include <cstddef>
#include <json/value.h>

struct ResponseBiometricEraseDto
{
  std::size_t faces{0};
  std::size_t portraits{0};
  bool voiceProfile{false};
  std::size_t voiceSamples{0};

  [[nodiscard]] Json::Value toJson() const;
};
