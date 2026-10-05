#pragma once

#include <grpc/grpc-server-identity.hxx>
#include <http/listener-config.hxx>

#include <string>
#include <utility>
#include <vector>

struct LlmRpcConfig
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
  std::vector<argus::client::CallerCredential> settingsCredentials;
};

struct LlmIdentityConfig
{
  std::string target;
  std::string credential;
  std::string rpcSecret;
};

struct LlmMemoryConfig
{
  bool observeCameraEvents{false};
};

struct LlmNotificationConfig
{
  std::string target;
  std::string credential;
};

class LlmConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static LlmRpcConfig resolveRpc();

  [[nodiscard]] static LlmIdentityConfig resolveIdentity();

  [[nodiscard]] static std::string resolveCameraTarget();
  [[nodiscard]] static std::string resolveCameraCredential();

  [[nodiscard]] static LlmMemoryConfig resolveMemory();

  [[nodiscard]] static LlmNotificationConfig resolveNotifications();
};
