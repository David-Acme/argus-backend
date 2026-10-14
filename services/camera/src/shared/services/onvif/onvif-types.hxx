#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class OnvifFailure : uint8_t
{
  None = 0,
  Transport,
  Unauthorized,
  Fault,
  Malformed,
  Unsupported
};

inline std::string onvifFailureToString(OnvifFailure failure)
{
  switch (failure) {
    case OnvifFailure::None:
      return "none";
    case OnvifFailure::Transport:
      return "transport";
    case OnvifFailure::Unauthorized:
      return "unauthorized";
    case OnvifFailure::Fault:
      return "fault";
    case OnvifFailure::Malformed:
      return "malformed";
    case OnvifFailure::Unsupported:
      return "unsupported";
  }
  return "none";
}

inline OnvifFailure onvifFailureFromString(std::string_view text)
{
  if (text == "transport")
    return OnvifFailure::Transport;
  if (text == "unauthorized")
    return OnvifFailure::Unauthorized;
  if (text == "fault")
    return OnvifFailure::Fault;
  if (text == "malformed")
    return OnvifFailure::Malformed;
  if (text == "unsupported")
    return OnvifFailure::Unsupported;
  return OnvifFailure::None;
}

template <typename T>
struct OnvifOutcome
{
  T value{};
  OnvifFailure failure{OnvifFailure::None};
  std::string error{};

  [[nodiscard]] bool ok() const { return failure == OnvifFailure::None; }

  static OnvifOutcome failed(OnvifFailure kind, const std::string& message)
  {
    return {.value = T{}, .failure = kind, .error = message};
  }
};

struct OnvifEndpoint
{
  std::string host;
  int port{80};
  std::string username;
  std::string password;
  double timeoutSeconds{5.0};
};

struct OnvifServices
{
  std::string device{"/onvif/device_service"};
  std::string media;
  std::string ptz;
  std::string imaging;
  std::string events;
};

struct OnvifDeviceInfo
{
  std::string manufacturer;
  std::string model;
  std::string firmware;
  std::string serial;
  std::string hardwareId;
};

struct OnvifProfile
{
  std::string token;
  std::string name;
  std::string ptzConfiguration;
  int width{0};
  int height{0};
};

struct OnvifPreset
{
  std::string token;
  std::string name;
};

struct OnvifVector
{
  double x{0.0};
  double y{0.0};
};

struct OnvifDevice
{
  OnvifDeviceInfo info;
  OnvifServices services;
  std::string profileToken;
  bool ptz{false};
  bool presets{false};
  int64_t clockOffsetSeconds{0};
};

struct OnvifSubscription
{
  std::string address;
  std::string path;
  std::string references;
  int64_t lifetimeSeconds{0};
};

struct OnvifMotion
{
  bool moving{false};
};

struct OnvifPull
{
  std::vector<OnvifMotion> motions;
  int64_t lifetimeSeconds{0};
};
