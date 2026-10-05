#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

struct LiveKitGrant
{
  std::string room;
  bool roomJoin{false};
  bool roomAdmin{false};
  bool roomList{false};
  bool roomCreate{false};
  bool canPublish{false};
  std::vector<std::string> publishSources;
  bool canSubscribe{false};
  bool canPublishData{false};
  bool canUpdateOwnMetadata{false};
};

struct LiveKitTokenInput
{
  std::string_view apiKey;
  std::string_view apiSecret;
  std::string identity;
  std::string kind;
  LiveKitGrant grant;
  std::chrono::seconds ttl{600};
  std::chrono::system_clock::time_point now;
};

[[nodiscard]] std::string mintLiveKitToken(const LiveKitTokenInput& input);
