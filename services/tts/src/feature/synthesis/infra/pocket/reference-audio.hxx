#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

struct ReferenceAudioInput
{
  std::filesystem::path directory;
  std::string_view name;
  int targetRate{24000};
};

[[nodiscard]] bool isReferenceName(std::string_view name);

[[nodiscard]] std::optional<std::filesystem::path> referencePath(const ReferenceAudioInput& input);

[[nodiscard]] std::vector<float> loadReferenceAudio(const ReferenceAudioInput& input);
