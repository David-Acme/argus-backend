#pragma once

#include <settings/component-host.hxx>

#include <filesystem>
#include <functional>
#include <string_view>

inline constexpr std::string_view kTtsComponent = "voice-tts";

struct TtsComponentsInput
{
  std::filesystem::path modelsDir;
  std::function<bool()> loaded;
};

[[nodiscard]] DiskComponentHostInput ttsComponents(TtsComponentsInput input);
