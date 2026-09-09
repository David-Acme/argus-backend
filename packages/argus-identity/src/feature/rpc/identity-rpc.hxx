#pragma once

#include <argus/identity/v1/identity.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <shared/repositories/device-credential/device-credential-repository.hxx>
#include <shared/repositories/refresh-token/refresh-token-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/jwt/jwt-service.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>

// IdentityService controller: UpdateUser is metadata-authoritative, the rest self-authoritative.
class IdentityRpcService final
    : public argus::identity::v1::IdentityService::CallbackService
{
public:
  // fleetSecret is required in x-argus-fleet on every call; empty allows only loopback.
  IdentityRpcService(std::shared_ptr<NatsBus> bus, std::string fleetSecret);

  grpc::ServerUnaryReactor*
  UpdateUser(grpc::CallbackServerContext* context,
             const argus::identity::v1::UpdateUserRequest* request,
             argus::identity::v1::UpdateUserResponse* response) override;

  grpc::ServerUnaryReactor*
  ValidateToken(grpc::CallbackServerContext* context,
                const argus::identity::v1::ValidateTokenRequest* request,
                argus::identity::v1::ValidateTokenResponse* response) override;

  grpc::ServerUnaryReactor*
  CheckDeviceCredential(
      grpc::CallbackServerContext* context,
      const argus::identity::v1::CheckDeviceCredentialRequest* request,
      argus::identity::v1::CheckDeviceCredentialResponse* response) override;

private:
  // Rejected validation: RPC OK with valid=false and the caller's 401 body.
  static void
  finishRejected(grpc::ServerUnaryReactor* reactor,
                 argus::identity::v1::ValidateTokenResponse* response,
                 const std::string& reason);

  // Fleet-secret gate; the listener is cleartext.
  bool fleetAuthorized(const grpc::CallbackServerContext* context) const;

  JwtService jwtService_;
  UserRepository userRepository_;
  RefreshTokenRepository refreshTokenRepository_;
  DeviceCredentialRepository deviceCredentialRepository_;
  std::shared_ptr<NatsBus> bus_;
  std::string fleetSecret_;
};
