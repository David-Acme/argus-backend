#pragma once

#include <cstdint>
#include <string>

struct IdentityDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct IdentityRpcConfig
{
  std::string host;
  uint16_t port{0};
  std::string secret;

  static IdentityRpcConfig resolve();

  bool reachableBeyondLoopback() const;
};

class IdentityConfig
{
public:
  static IdentityDbConfig resolveDb();
};
