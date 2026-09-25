#pragma once

#include <http/listener-config.hxx>

#include <string>
#include <utility>
#include <vector>

struct LlmRpcConfig
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
};

struct LlmIdentityConfig
{
  std::string target;
  std::string rpcSecret;
};

struct LlmMemoryConfig
{
  bool observeCameraEvents{false};
};

class LlmConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static LlmRpcConfig resolveRpc();

  [[nodiscard]] static LlmIdentityConfig resolveIdentity();

  [[nodiscard]] static std::string resolveCameraTarget();

  [[nodiscard]] static LlmMemoryConfig resolveMemory();
};
