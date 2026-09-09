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

  // A non-loopback listener answers verdicts for the whole fleet: must be authed.
  bool reachableBeyondLoopback() const;
};

class IdentityConfig
{
public:
  // Resolves [identity] db / [identity] schema with the phase-1 defaults.
  static IdentityDbConfig resolveDb();
};
