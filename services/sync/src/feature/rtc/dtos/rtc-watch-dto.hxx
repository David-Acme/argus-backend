#pragma once

#include <string>

[[nodiscard]] bool isWatchConversationId(const std::string& value);

struct RtcWatchEndDto
{
  std::string conversationId;

  static RtcWatchEndDto fromPath(const std::string& id);
};
