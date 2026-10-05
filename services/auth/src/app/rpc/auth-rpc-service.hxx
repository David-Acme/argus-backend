#pragma once

#include <argus/auth/v1/auth.grpc.pb.h>
#include <feature/device/repositories/device-credential/device-credential-repository.hxx>
#include <feature/session/services/session-service.hxx>
#include <grpc/fleet-caller-gate.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

class AuthRpcService final : public argus::auth::v1::AuthService::CallbackService
{
public:
  struct Dependencies
  {
    SessionService* sessions{nullptr};
    DeviceCredentialRepository* deviceCredentials{nullptr};
  };

  AuthRpcService(Dependencies dependencies,
                 std::shared_ptr<const argus::client::FleetCallerGate> gate);

  AuthRpcService(Dependencies dependencies, std::string fleetSecret);

  grpc::ServerUnaryReactor*
  ValidateToken(grpc::CallbackServerContext* context,
                const argus::auth::v1::ValidateTokenRequest* request,
                argus::auth::v1::ValidateTokenResponse* response) override;

  grpc::ServerUnaryReactor*
  CheckDeviceCredential(
      grpc::CallbackServerContext* context,
      const argus::auth::v1::CheckDeviceCredentialRequest* request,
      argus::auth::v1::CheckDeviceCredentialResponse* response) override;

private:
  [[nodiscard]] grpc::ServerUnaryReactor*
  refuseCaller(grpc::CallbackServerContext* context) const;

  Dependencies dependencies_;
  std::shared_ptr<const argus::client::FleetCallerGate> gate_;
};
