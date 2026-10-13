#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tunnel::link
{
enum class ClientHelloStatus : uint8_t
{
  NeedMore,
  Parsed,
  Malformed,
};

struct ClientHelloResult
{
  ClientHelloStatus status{ClientHelloStatus::NeedMore};
  std::string serverName;
};

struct DeviceName
{
  std::string service;
  std::string homeId;
};

struct DeviceNameInput
{
  std::string_view serverName;
  std::string_view domainSuffix;
};

inline constexpr std::string_view kDeviceDomainSuffix = ".argus-tunnel.lynk.page";
inline constexpr size_t kClientHelloMaxBytes = 32UL * 1024UL;
inline constexpr uint8_t kUnrecognizedNameAlert = 112;

ClientHelloResult parseClientHello(std::string_view bytes);
std::optional<DeviceName> parseDeviceName(const DeviceNameInput& input);
std::string tlsAlertRecord(uint8_t description);
}
