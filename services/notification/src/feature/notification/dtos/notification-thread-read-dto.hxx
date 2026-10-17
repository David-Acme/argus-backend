#pragma once

#include <cstddef>
#include <string>

struct NotificationThreadReadDto
{
  static constexpr std::size_t kMaxThreadKeyLength = 128;

  std::string threadKey;

  static NotificationThreadReadDto fromPath(std::string threadKey);
};
