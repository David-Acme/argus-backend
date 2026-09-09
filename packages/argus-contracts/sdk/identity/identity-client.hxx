#pragma once

#include <argus/identity/v1/identity.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>

// Typed spoken-name write input; role rides the x-argus-role metadata.
struct UpdateUserNameInput
{
  int64_t userId{0};
  std::string name;
  std::string role;
};

// Token validation input. hasDeviceContext mirrors the device filter having
// run: an EMPTY hash with the context present is a failed credential and must
// still be checked against the session's binding, so presence and value are
// carried separately.
struct ValidateTokenInput
{
  std::string accessToken;
  std::string deviceHash;
  bool hasDeviceContext{false};
};

// Thin SDK wrapper over argus.identity.v1.IdentityService (rule 23).
class IdentityClient
{
public:
  // fleetSecret rides every call as x-argus-fleet; empty means the identity
  // service must be on a loopback listener to accept the call.
  explicit IdentityClient(std::string target, std::string fleetSecret = {});

  IdentityClient(const IdentityClient&) = delete;
  IdentityClient& operator=(const IdentityClient&) = delete;
  virtual ~IdentityClient() = default;

  // Spoken-name write; nullopt when the gateway refuses or is unreachable.
  virtual std::optional<argus::identity::v1::UserIdentity>
  updateUserName(const UpdateUserNameInput& input) const;

  // Server-authoritative token validation; nullopt when the gateway is
  // unreachable (the caller must treat that as a rejection).
  virtual std::optional<argus::identity::v1::ValidateTokenResponse>
  validateToken(const ValidateTokenInput& input) const;

  // Device credential check by secret hash; false when unknown, refused or
  // unreachable.
  virtual bool checkDeviceCredential(const std::string& secretHash) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::identity::v1::IdentityService::StubInterface>
      stub_;
  std::string fleetSecret_;
};
