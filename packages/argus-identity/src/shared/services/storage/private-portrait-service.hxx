#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>

struct PrivatePortrait
{
  std::string mimeType;
  std::string bytes;
};

class PrivatePortraitService
{
public:
  drogon::Task<void> store(int64_t userId, const std::string& image) const;
  drogon::Task<bool> has(int64_t userId) const;
  drogon::Task<std::optional<PrivatePortrait>> read(int64_t userId) const;
};
