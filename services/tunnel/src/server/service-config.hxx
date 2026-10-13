#pragma once

#include <client/tunnel-client.hxx>
#include <link/client/link-client.hxx>
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
  bool linkEnabled{false};
  std::string homeId;
  tunnel::ClientOptions tunnel;
  tunnel::link::LinkOptions link;

  static ClientConfig resolve();
};

std::optional<std::string> linkConfigProblem(const ClientConfig& config);

struct RelayConfig
{
  std::string healthHost{"0.0.0.0"};
  uint16_t healthPort{7103};
  tunnel::RelayOptions relay;

  static RelayConfig resolve();
};
