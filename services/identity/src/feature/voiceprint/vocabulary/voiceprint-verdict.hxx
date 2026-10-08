#pragma once

#include <cstdint>

enum class VoiceprintVerdict : uint8_t
{
  Unknown = 0,
  Holder,
  OtherKnown,
  Unfamiliar
};
