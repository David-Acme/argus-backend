#pragma once

#include <json/value.h>

#include <string>
#include <string_view>

namespace pairing_banner
{
struct PairingBannerInput
{
  std::string code;
  std::string serverName;
  int port{0};
  std::string instanceId;
  std::string caFingerprint;
  std::string serverFingerprint;
};

[[nodiscard]] Json::Value payload(const PairingBannerInput& input);

[[nodiscard]] std::string asciiQr(std::string_view text);

[[nodiscard]] std::string render(const PairingBannerInput& input);

void printWhenUnpaired(int port);
}
