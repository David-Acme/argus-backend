#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace secret_box
{
inline constexpr std::string_view kPrefix = "enc:v1:";
inline constexpr std::size_t kKeyBytes = 32;

struct SealInput
{
  std::string_view plain;
  std::string_view label;
};

struct OpenInput
{
  std::string_view stored;
  std::string_view label;
};

enum class KeyFileResult : uint8_t
{
  Loaded = 0,
  Created,
  Failed
};

bool installKey(std::span<const uint8_t> key);
void clearKey();
[[nodiscard]] bool hasKey();
[[nodiscard]] KeyFileResult loadOrCreateKey(const std::string& path);

[[nodiscard]] bool isSealed(std::string_view stored);
[[nodiscard]] std::string seal(const SealInput& input);
[[nodiscard]] std::string open(const OpenInput& input);
}
