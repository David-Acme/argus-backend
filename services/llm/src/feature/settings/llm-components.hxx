#pragma once

#include <settings/component-host.hxx>

#include <filesystem>
#include <functional>
#include <string_view>

inline constexpr std::string_view kLlmComponent = "llm";

struct LlmComponentsInput
{
  std::filesystem::path modelsDir;
  std::function<bool()> loaded;
};

[[nodiscard]] DiskComponentHostInput llmComponents(LlmComponentsInput input);
