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

  static IdentityRpcConfig resolve();
};

class IdentityConfig
{
public:
  // Resolves [identity] db / [identity] schema with the Fase-1 defaults.
  static IdentityDbConfig resolveDb();
};
