#pragma once

#include <client/tunnel-client.hxx>
#include <relay/tunnel-relay.hxx>

#include <json/value.h>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

inline constexpr std::size_t kMinTunnelSecretBytes = 32;

std::optional<std::string> tunnelSecretProblem(std::string_view secret);

struct ClientConfig
{
  std::string healthHost{"127.0.0.1"};
  uint16_t healthPort{7104};
  tunnel::ClientOptions tunnel;

  static ClientConfig resolve();
};

struct RelayConfig
{
  std::string healthHost{"0.0.0.0"};
  uint16_t healthPort{7103};
  tunnel::RelayOptions relay;

  static RelayConfig resolve();
};
