#pragma once

#include <cstdint>
#include <string>

enum class VoiceprintMethod : uint8_t
{
  Self = 0,
  OwnerFace
};

inline std::string voiceprintMethodToString(VoiceprintMethod method)
{
  return method == VoiceprintMethod::OwnerFace ? "owner_face" : "self";
}

inline VoiceprintMethod voiceprintMethodFromString(const std::string& value)
{
  return value == "owner_face" ? VoiceprintMethod::OwnerFace
                               : VoiceprintMethod::Self;
}
