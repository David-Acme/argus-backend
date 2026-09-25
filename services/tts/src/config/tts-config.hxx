#pragma once

#include <http/listener-config.hxx>

#include <string>
#include <utility>
#include <vector>

struct TtsRpcConfig
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
};

class TtsConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static TtsRpcConfig resolveRpc();
};
