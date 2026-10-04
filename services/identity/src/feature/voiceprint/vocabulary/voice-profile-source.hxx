#pragma once

#include <cstdint>
#include <string>

enum class VoiceProfileSource : uint8_t
{
  Passive = 0,
  Enrolled
};

inline std::string voiceProfileSourceToString(VoiceProfileSource source)
{
  return source == VoiceProfileSource::Enrolled ? "enrolled" : "passive";
}

inline VoiceProfileSource voiceProfileSourceFromString(const std::string& value)
{
  return value == "enrolled" ? VoiceProfileSource::Enrolled
                             : VoiceProfileSource::Passive;
}
