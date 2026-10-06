#pragma once

#include <grpc/grpc-server-identity.hxx>
#include <http/listener-config.hxx>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct SttRpcConfig
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
  std::vector<argus::client::CallerCredential> settingsCredentials;
};

class SttConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static SttRpcConfig resolveRpc();

  [[nodiscard]] static std::filesystem::path resolveComponentsRoot();
};
