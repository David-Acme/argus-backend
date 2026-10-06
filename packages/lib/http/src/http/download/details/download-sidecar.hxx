#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace file_download::details
{

struct Sidecar
{
  std::string url;
  std::string finalUrl;
  std::uint64_t size = 0;
  std::string sha256;
  std::string etag;
  std::uint64_t bytes = 0;
};

[[nodiscard]] std::optional<Sidecar> readSidecar(const std::filesystem::path& path);

[[nodiscard]] bool writeSidecar(const std::filesystem::path& path, const Sidecar& sidecar);

}
