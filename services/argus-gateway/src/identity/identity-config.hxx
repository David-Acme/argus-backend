#pragma once

#include <cstdint>
#include <string>

struct IdentityDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

// The internal identity gRPC listener config.
struct IdentityRpcConfig
{
  std::string host;
  uint16_t port{0};
  std::string secret;

  static IdentityRpcConfig resolve();

  // A listener reachable beyond the loopback interface answers token and
  // credential verdicts for the whole fleet, so it may not run unauthenticated.
  bool reachableBeyondLoopback() const;
};

class IdentityConfig
{
public:
  // Resolves [identity] db / [identity] schema with the phase-1 defaults.
  static IdentityDbConfig resolveDb();
};
