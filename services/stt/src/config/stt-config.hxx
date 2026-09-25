#pragma once

#include <http/listener-config.hxx>

#include <string>
#include <utility>
#include <vector>

struct SttRpcConfig
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
};

class SttConfig
{
public:
  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static SttRpcConfig resolveRpc();
};
