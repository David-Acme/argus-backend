#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

// Canonical voice interaction languages (STT + TTS + prompts); the DB stores
// the string code.
enum class VoiceLang : uint8_t
{
  System = 0,
  Es,
  En
};

inline std::string voiceLangToString(VoiceLang lang)
{
  switch (lang) {
    case VoiceLang::Es:
      return "es";
    case VoiceLang::En:
      return "en";
    case VoiceLang::System:
      return "";
  }
  return "";
}

inline VoiceLang voiceLangFromString(const std::string& s)
{
  static const std::unordered_map<std::string, VoiceLang> kMap = {
      {"es", VoiceLang::Es},
      {"en", VoiceLang::En},
  };
  const auto it = kMap.find(s);
  return it == kMap.end() ? VoiceLang::System : it->second;
}
