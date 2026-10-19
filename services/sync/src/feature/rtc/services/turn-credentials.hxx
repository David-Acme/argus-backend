#pragma once

#include <config/sync-config.hxx>

#include <json/value.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct TurnCredentialInput
{
  std::string_view secret;
  std::string_view subject;
  int64_t expiresAt{0};
};

struct TurnCredential
{
  std::string username;
  std::string credential;
  int64_t expiresAt{0};
};

struct IceServersInput
{
  const SyncTurnConfig& relay;
  std::string_view subject;
  int64_t now{0};
};

struct IceServers
{
  Json::Value servers{Json::arrayValue};
  int64_t expiresAt{0};
};

namespace turn_credentials
{
[[nodiscard]] bool enabled(const SyncTurnConfig& relay);

[[nodiscard]] TurnCredential make(const TurnCredentialInput& input);

[[nodiscard]] std::vector<std::string> urls(const SyncTurnConfig& relay);

[[nodiscard]] IceServers iceServers(const IceServersInput& input);
}
