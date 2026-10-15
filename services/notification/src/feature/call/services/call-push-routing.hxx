#pragma once

#include <push/push-envelope.hxx>
#include <shared/services/push-delivery/push-device.hxx>

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
}
