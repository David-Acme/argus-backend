#pragma once

#include <client/tunnel-client.hxx>
#include <relay/tunnel-relay.hxx>

#include <json/value.h>
#include <string>

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
