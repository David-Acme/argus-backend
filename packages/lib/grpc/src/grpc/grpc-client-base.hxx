#pragma once

#include <cstdint>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <string>

namespace argus::client
{

struct CallerIdentity
{
  int64_t userId{0};
  std::string role;
  std::optional<std::string> device;
};

struct PeerCredential
{
  std::string credential;
  std::string fleetSecret;
};

std::shared_ptr<grpc::Channel> makeChannel(const std::string& target);

std::shared_ptr<grpc::Channel> makeStreamingChannel(const std::string& target);

void setDeadline(grpc::ClientContext& context, int timeoutMs);

void addCallerIdentity(grpc::ClientContext& context,
                       const CallerIdentity& identity);

void addFleetSecret(grpc::ClientContext& context, const std::string& secret);

void addCallerCredential(grpc::ClientContext& context,
                         const std::string& secret);

void addPeerCredential(grpc::ClientContext& context,
                       const PeerCredential& credential);

inline constexpr const char* kFleetSecretKey = "x-argus-fleet";

inline constexpr const char* kCallerCredentialKey = "x-argus-credential";

}
