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
#include <feature/auth/infra/client-identity.hxx>
#include <feature/auth/services/session-management-service.hxx>
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
  ClientIdentity client;
};

struct DeviceLoginStartInput
{
  LoginDeviceInput device;
  std::string pollHash;
};

struct DeviceLoginPollInput
{
  std::string challengeId;
  LoginDeviceInput device;
  std::string proof;
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
  std::string ip;
  std::string credentialHash;
  ClientIdentity client;
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
  std::string sessionId;
};

struct RefreshPresentation
{
  int64_t userId{0};
  std::string token;
  std::string tokenHash;
  std::string sessionId;
};

struct StaleRefreshInput
{
  const RefreshTokenSchema& session;
  const RefreshTokenInput& request;
  const std::string& tokenHash;
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
    SessionManagementService sessions;
    const IdentityClient* identity{nullptr};
  };

  struct Config
  {
    int64_t refreshReuseGraceSeconds{30};
  };

  AuthFeatureService(Dependencies dependencies, Config config);

  [[nodiscard]] drogon::Task<ResponseLoginDto>
  login(LoginDto body, const LoginDeviceInput& device) const;

  [[nodiscard]] drogon::Task<ResponseLoginDto>
  registerUser(RegisterDto body, const LoginDeviceInput& device) const;

  [[nodiscard]] drogon::Task<CreateDeviceLoginDto>
  createDeviceLogin(const DeviceLoginStartInput& input) const;

  [[nodiscard]] drogon::Task<void>
  approveDeviceLogin(const std::string& challengeId,
                     int64_t approvingUserId) const;

  [[nodiscard]] drogon::Task<DeviceLoginStatusDto>
  pollDeviceLogin(const DeviceLoginPollInput& input) const;

  [[nodiscard]] drogon::Task<ResponseRefreshTokenDto>
  refreshToken(const RefreshTokenInput& input) const;

  [[nodiscard]] drogon::Task<void> logout(const LogoutInput& input) const;

  [[nodiscard]] drogon::Task<void> updateMe(const UpdateMeInput& input) const;

private:
  [[nodiscard]] drogon::Task<ResponseLoginDto>
  issueSession(const IssueSessionInput& input) const;

  [[nodiscard]] drogon::Task<IssuedDeviceCredential>
  issueDeviceCredential(const IssueDeviceCredentialInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<RefreshTokenSchema>>
  presentedSession(const RefreshPresentation& presented) const;

  [[nodiscard]] drogon::Task<void>
  settleStaleToken(const StaleRefreshInput& input) const;

  [[nodiscard]] drogon::Task<void>
  refuseDisabledAccount(int64_t userId) const;

  Dependencies dependencies_;
  Config config_;
};
