#pragma once

#include <http/listener-config.hxx>

#include <string>
#include <utility>
#include <vector>

struct VlmRpcConfig
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
};

class VlmConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static VlmRpcConfig resolveRpc();
};
