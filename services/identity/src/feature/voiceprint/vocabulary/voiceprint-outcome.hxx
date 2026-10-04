#pragma once

#include <cstdint>

enum class VoiceprintOutcome : uint8_t
{
  Ok = 0,
  Unavailable,
  SampleInvalid,
  SampleTooShort,
  SampleTooNoisy,
  SampleClipped
};
