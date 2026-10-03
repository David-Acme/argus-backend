#pragma once

#include <argus/auth/v1/auth.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>

struct AuthClientConfig
{
  std::string target;
  std::string fleetSecret;
};

struct ValidateSessionInput
{
  std::string accessToken;
  std::string deviceHash;
  bool hasDeviceContext{false};
};

class AuthClient
{
public:
  explicit AuthClient(AuthClientConfig config);

  AuthClient(const AuthClient&) = delete;
  AuthClient& operator=(const AuthClient&) = delete;
  virtual ~AuthClient() = default;

  [[nodiscard]] virtual std::optional<argus::auth::v1::ValidateTokenResponse>
  validateToken(const ValidateSessionInput& input) const;

  [[nodiscard]] virtual std::optional<bool>
  checkDeviceCredential(const std::string& secretHash) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::auth::v1::AuthService::StubInterface> stub_;
  std::string fleetSecret_;
};
