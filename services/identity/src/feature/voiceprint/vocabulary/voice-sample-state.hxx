#pragma once

#include <cstdint>
#include <string>

enum class VoiceSampleState : uint8_t
{
  Pending = 0,
  Adopted
};

inline std::string voiceSampleStateToString(VoiceSampleState state)
{
  return state == VoiceSampleState::Adopted ? "adopted" : "pending";
}

inline VoiceSampleState voiceSampleStateFromString(const std::string& value)
{
  return value == "adopted" ? VoiceSampleState::Adopted
                            : VoiceSampleState::Pending;
}
