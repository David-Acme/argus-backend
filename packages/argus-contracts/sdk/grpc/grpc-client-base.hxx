#pragma once

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace argus::sdk
{

// The caller the RPC acts for, carried as x-argus-* metadata. Receivers
// check PRESENCE, not value (argus-camera's sync service rejects a request
// missing any of the three keys), so an engaged-but-empty device is sent as
// an empty header rather than dropped.
struct CallerIdentity
{
  int64_t userId{0};
  std::string role;
  // Engaged when the contract carries a device, whatever its value.
  std::optional<std::string> device;
};

// The one place channel credentials are chosen for every SDK client.
std::shared_ptr<grpc::Channel> makeChannel(const std::string& target);

// Same, plus the HTTP/2 keepalive a long-lived stream needs.
std::shared_ptr<grpc::Channel> makeStreamingChannel(const std::string& target);

void setDeadline(grpc::ClientContext& context, int timeoutMs);

void addCallerIdentity(grpc::ClientContext& context,
                       const CallerIdentity& identity);

// The fleet-shared secret proving the caller is part of this installation,
// sent as x-argus-fleet. An empty secret sends no header, which the receiver
// only accepts on a loopback listener.
void addFleetSecret(grpc::ClientContext& context, const std::string& secret);

// The metadata key the fleet secret travels in, shared by both ends.
inline constexpr const char* kFleetSecretKey = "x-argus-fleet";

} // namespace argus::sdk
