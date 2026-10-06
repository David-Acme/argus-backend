#pragma once

#include <grpc/grpc-server-identity.hxx>
#include <http/listener-config.hxx>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct TtsRpcConfig
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
  std::vector<argus::client::CallerCredential> settingsCredentials;
};

class TtsConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static TtsRpcConfig resolveRpc();

  [[nodiscard]] static std::filesystem::path resolveComponentsRoot();
};
