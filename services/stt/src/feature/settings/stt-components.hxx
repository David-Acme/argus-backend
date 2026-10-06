#pragma once

#include <settings/component-host.hxx>

#include <filesystem>
#include <functional>
#include <string_view>

inline constexpr std::string_view kSttComponent = "voice-stt";

struct SttComponentsInput
{
  std::filesystem::path modelsDir;
  std::function<bool()> loaded;
};

[[nodiscard]] DiskComponentHostInput sttComponents(SttComponentsInput input);
