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

struct SttSpotConfig
{
  std::string modelDir;
  std::string encoder;
  std::string decoder;
  std::string joiner;
  std::string tokens;
  std::string keywordsFile;
  float threshold{0.25F};
  float score{1.5F};
  int maxActivePaths{4};
  int trailingBlanks{1};
  int threads{1};
};

class SttConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static SttRpcConfig resolveRpc();

  [[nodiscard]] static std::filesystem::path resolveComponentsRoot();

  [[nodiscard]] static SttSpotConfig resolveSpot();
};
