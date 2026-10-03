#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct ProsodicChunkInput
{
  std::string_view text;
  std::size_t maxUnits{0};
  std::function<std::size_t(std::string_view)> measure;
};

[[nodiscard]] std::vector<std::string> chunkProsodic(const ProsodicChunkInput& input);

[[nodiscard]] std::size_t codepointCount(std::string_view text);
