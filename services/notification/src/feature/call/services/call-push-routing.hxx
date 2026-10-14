#pragma once

#include <feature/call/services/call-ports.hxx>
#include <push/push-envelope.hxx>

#include <vector>

namespace call_push_routing
{
inline std::vector<CallPushDevice> devicesForCall(const std::vector<CallPushDevice>& devices)
{
  std::vector<CallPushDevice> routed;
  routed.reserve(devices.size());
  for (const auto& device : devices) {
    if (push_envelope::combinationAllowed(device.provider, PushKind::Call))
      routed.push_back(device);
  }
  return routed;
}
}
