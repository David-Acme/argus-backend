#pragma once

#include <cstdint>
#include <string_view>

enum class VoiceSpeakerVerdict : uint8_t
{
  Unknown,
  Holder,
  OtherKnown,
  Unfamiliar
};

[[nodiscard]] constexpr std::string_view voiceSpeakerVerdictToString(VoiceSpeakerVerdict verdict)
{
  switch (verdict) {
    case VoiceSpeakerVerdict::Holder:
      return "holder";
    case VoiceSpeakerVerdict::OtherKnown:
      return "other_known";
    case VoiceSpeakerVerdict::Unfamiliar:
      return "unfamiliar";
    case VoiceSpeakerVerdict::Unknown:
      return "unknown";
  }
  return "unknown";
}
