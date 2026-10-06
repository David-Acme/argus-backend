#pragma once

#include <auth/auth-client.hxx>
#include <feature/device/repositories/device-credential/device-credential-repository.hxx>
#include <feature/session/services/session-service.hxx>

#include <chrono>
#include <optional>
#include <string>

class LocalAuthClient final : public AuthClient
{
public:
  struct Dependencies
  {
    const SessionService* sessions{nullptr};
    const DeviceCredentialRepository* deviceCredentials{nullptr};
    std::chrono::milliseconds timeout{5000};
  };

  explicit LocalAuthClient(Dependencies dependencies);

  [[nodiscard]] std::optional<argus::auth::v1::ValidateTokenResponse>
  validateToken(const ValidateSessionInput& input) const override;

  [[nodiscard]] std::optional<bool>
  checkDeviceCredential(const std::string& secretHash) const override;

  static void install(Dependencies dependencies);

private:
  Dependencies dependencies_;
};
