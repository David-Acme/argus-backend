#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>
#include <string_view>

inline constexpr std::string_view kPortraitMimeType = "image/jpeg";

struct PrivatePortrait
{
  std::string mimeType;
  std::string bytes;
};

class PrivatePortraitService
{
public:
  drogon::Task<void> store(int64_t userId, const std::string& portraitJpeg) const;
  drogon::Task<bool> has(int64_t userId) const;
  drogon::Task<std::optional<PrivatePortrait>> read(int64_t userId) const;
};
