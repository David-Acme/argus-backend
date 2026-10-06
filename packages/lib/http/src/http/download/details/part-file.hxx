#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace file_download::details
{

struct PartOpen;

class PartFile
{
public:
  PartFile() = default;
  PartFile(const PartFile&) = delete;
  PartFile& operator=(const PartFile&) = delete;
  PartFile(PartFile&& other) noexcept;
  PartFile& operator=(PartFile&& other) noexcept;
  ~PartFile();

  [[nodiscard]] static PartOpen open(const std::filesystem::path& path);

  [[nodiscard]] std::optional<std::uint64_t> size() const;
  [[nodiscard]] bool truncate(std::uint64_t length) const;
  [[nodiscard]] bool append(std::span<const char> bytes) const;
  [[nodiscard]] bool sync() const;
  void close();
  [[nodiscard]] bool isOpen() const { return descriptor_ >= 0; }

private:
  explicit PartFile(int descriptor) : descriptor_(descriptor) {}

  int descriptor_ = -1;
};

struct PartOpen
{
  PartFile file;
  bool busy = false;
  std::string error;
};

[[nodiscard]] std::string lastSystemError();

[[nodiscard]] bool syncDirectory(const std::filesystem::path& directory);

}
