#pragma once

#include <push/push-envelope.hxx>
#include <shared/services/push-delivery/push-device.hxx>

#include <string>
#include <vector>

namespace call_push_routing
{
inline std::vector<PushDevice> devicesForCall(const std::vector<PushDevice>& devices)
{
  std::vector<PushDevice> routed;
  routed.reserve(devices.size());
  for (const auto& device : devices) {
    if (push_envelope::combinationAllowed(device.provider, PushKind::Call))
      routed.push_back(device);
  }
  return routed;
}

inline std::vector<PushDevice> devicesExceptSession(const std::vector<PushDevice>& devices,
                                                    const std::string& sessionId)
{
  if (sessionId.empty())
    return devices;
  std::vector<PushDevice> others;
  others.reserve(devices.size());
  for (const auto& device : devices) {
    if (device.sessionId != sessionId)
      others.push_back(device);
  }
  return others;
}
}
