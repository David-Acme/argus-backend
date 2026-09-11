#pragma once

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <optional>
#include <string>

namespace argus::sdk
{

// Reads the x-argus-user / x-argus-role / x-argus-device metadata the client
// base attaches; nullopt when any leg is missing or the user id is invalid.
inline std::optional<int64_t>
callerUserId(const grpc::CallbackServerContext* context)
{
  bool user = false;
  bool role = false;
  bool device = false;
  int64_t id = 0;
  for (const auto& [key, value] : context->client_metadata()) {
    if (key == "x-argus-user") {
      user = true;
      try {
        id = std::stoll(std::string(value.data(), value.size()));
      }
      catch (const std::exception&) {
        return std::nullopt;
      }
    }
    else if (key == "x-argus-role")
      role = true;
    else if (key == "x-argus-device")
      device = true;
  }
  if (!(user && role && device))
    return std::nullopt;
  return id;
}

} // namespace argus::sdk
