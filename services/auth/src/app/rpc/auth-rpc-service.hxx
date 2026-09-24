#pragma once

#include <argus/auth/v1/auth.grpc.pb.h>
#include <feature/device/repositories/device-credential/device-credential-repository.hxx>
#include <feature/session/services/session-service.hxx>
#include <grpcpp/grpcpp.h>
#include <string>

class AuthRpcService final : public argus::auth::v1::AuthService::CallbackService
{
public:
  struct Dependencies
  {
    SessionService* sessions{nullptr};
    DeviceCredentialRepository* deviceCredentials{nullptr};
  };

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
  [[nodiscard]] bool
  fleetAuthorized(const grpc::CallbackServerContext* context) const;

  Dependencies dependencies_;
  std::string fleetSecret_;
};
