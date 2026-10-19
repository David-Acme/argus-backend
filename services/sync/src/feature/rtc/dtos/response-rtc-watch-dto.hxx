#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <json/value.h>

#include <vector>

[[nodiscard]] Json::Value watchConversationJson(const argus::voice::v1::WatchConversation& conversation);

struct ResponseRtcWatchDto
{
  std::vector<argus::voice::v1::WatchConversation> conversations;

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponseRtcWatchEndDto
{
  bool ended{true};

  [[nodiscard]] Json::Value toJson() const;
};
