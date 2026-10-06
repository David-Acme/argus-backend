#pragma once

#include <grpc/grpc-server-identity.hxx>
#include <http/listener-config.hxx>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

struct LlmDecisionConfig
{
  double act{0.0};
  double ask{0.0};
  double margin{0.0};
};

struct LlmNotificationConfig
{
  std::string target;
  std::string credential;
};

struct LlmToolProviderConfig
{
  std::string id;
  std::string target;
  std::string credential;
};

class LlmConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static LlmRpcConfig resolveRpc();

  [[nodiscard]] static std::filesystem::path resolveComponentsRoot();

  [[nodiscard]] static LlmIdentityConfig resolveIdentity();

  [[nodiscard]] static std::string resolveCameraTarget();
  [[nodiscard]] static std::string resolveCameraCredential();

  [[nodiscard]] static LlmMemoryConfig resolveMemory();

  [[nodiscard]] static std::optional<LlmDecisionConfig> resolveDecision(std::string_view decider = {});

  [[nodiscard]] static bool resolveWitnessOnly(std::string_view decider);

  [[nodiscard]] static LlmNotificationConfig resolveNotifications();

  [[nodiscard]] static std::vector<LlmToolProviderConfig> resolveToolProviders();
};
