#pragma once

#include <auth/jwt-service.hxx>
#include <auth/user-role.hxx>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/auth/dtos/create-device-login-dto.hxx>
#include <feature/auth/dtos/device-login-status-dto.hxx>
#include <feature/auth/dtos/login-dto.hxx>
#include <feature/auth/dtos/refresh-token-dto.hxx>
#include <feature/auth/dtos/register-dto.hxx>
#include <feature/auth/dtos/response-login-dto.hxx>
#include <feature/auth/dtos/response-refresh-token-dto.hxx>
#include <feature/device/repositories/device-credential/device-credential-repository.hxx>
#include <feature/device/repositories/device-login-challenge/device-login-challenge-repository.hxx>
#include <feature/session/repositories/refresh-token/refresh-token-repository.hxx>
#include <optional>
#include <string>

class IdentityClient;

struct LoginDeviceInput
{
  std::string deviceHash;
  std::string userAgent;
};

struct SessionUser
{
  int64_t userId{0};
  std::string name;
  std::string lastName;
  UserRole role{UserRole::Guest};
};

struct IssuedDeviceCredential
{
  std::string secret;
  std::string deviceHash;
};

struct RefreshTokenInput
{
  RefreshTokenDto body;
  std::string deviceHash;
  std::string userAgent;
};

struct IssueSessionInput
{
  SessionUser user;
  int64_t personId{0};
  LoginDeviceInput device;
};

struct IssueDeviceCredentialInput
{
  int64_t userId{0};
  std::string userAgent;
  drogon::orm::DbClient* client{nullptr};
};

struct UpdateMeInput
{
  int64_t userId{0};
  std::string role;
  std::optional<std::string> name;
};

struct LogoutInput
{
  int64_t userId{0};
  std::string name;
  UserRole role{UserRole::Guest};
};

class AuthFeatureService
{
public:
  struct Dependencies
  {
    JwtService jwtService;
    RefreshTokenRepository refreshTokenRepository;
    DeviceCredentialRepository deviceCredentialRepository;
    DeviceLoginChallengeRepository challengeRepository;
    const IdentityClient* identity{nullptr};
  };

  explicit AuthFeatureService(Dependencies dependencies);

  [[nodiscard]] drogon::Task<ResponseLoginDto>
  login(LoginDto body, const LoginDeviceInput& device) const;

  [[nodiscard]] drogon::Task<ResponseLoginDto>
  registerUser(RegisterDto body, const LoginDeviceInput& device) const;

  [[nodiscard]] drogon::Task<CreateDeviceLoginDto>
  createDeviceLogin(const LoginDeviceInput& device) const;

  [[nodiscard]] drogon::Task<void>
  approveDeviceLogin(const std::string& challengeId,
                     int64_t approvingUserId) const;

  [[nodiscard]] drogon::Task<DeviceLoginStatusDto>
  pollDeviceLogin(const std::string& challengeId) const;

  [[nodiscard]] drogon::Task<ResponseRefreshTokenDto>
  refreshToken(const RefreshTokenInput& input) const;

  [[nodiscard]] drogon::Task<void> logout(const LogoutInput& input) const;

  [[nodiscard]] drogon::Task<void> updateMe(const UpdateMeInput& input) const;

private:
  [[nodiscard]] drogon::Task<ResponseLoginDto>
  issueSession(const IssueSessionInput& input) const;

  [[nodiscard]] drogon::Task<IssuedDeviceCredential>
  issueDeviceCredential(const IssueDeviceCredentialInput& input) const;

  Dependencies dependencies_;
};
