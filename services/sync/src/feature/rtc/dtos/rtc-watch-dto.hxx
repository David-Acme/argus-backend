#pragma once

#include <json/value.h>

#include <string>

[[nodiscard]] bool isWatchConversationId(const std::string& value);

struct RtcWatchEndDto
{
  std::string conversationId;

  static RtcWatchEndDto fromPath(const std::string& id);
};

struct RtcWatchReplyDto
{
  std::string conversationId;
  std::string text;

  static RtcWatchReplyDto fromJson(const Json::Value& json, const std::string& id);
};
