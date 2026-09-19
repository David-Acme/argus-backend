#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/auth/dtos/create-device-login-dto.hxx>
#include <feature/api/auth/dtos/device-login-status-dto.hxx>
#include <feature/api/auth/dtos/login-dto.hxx>
#include <feature/api/auth/dtos/refresh-token-dto.hxx>
#include <feature/api/auth/dtos/register-dto.hxx>
#include <feature/api/auth/dtos/response-login-dto.hxx>
#include <feature/api/auth/dtos/response-refresh-token-dto.hxx>
#include <shared/repositories/device-credential/device-credential-repository.hxx>
#include <shared/repositories/device-login-challenge/device-login-challenge-repository.hxx>
#include <shared/repositories/face-embedding/face-embedding-repository.hxx>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/refresh-token/refresh-token-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/storage/private-portrait-service.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <shared/services/jwt/jwt-service.hxx>
#include <shared/services/socket/socket-service.hxx>
#include <shared/services/sync-audit/sync-audit-service.hxx>
#include <shared/services/user-action-log/user-action-log-service.hxx>
#include <string>

struct LoginDeviceInput
{
  std::string deviceHash;
  std::string userAgent;
};

// Plaintext device secret shown once to the client plus its device hash.
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
  int64_t userId{0};
  int64_t personId{0};
  UserSchema user;
  LoginDeviceInput device;
};

class AuthService
{
public:
  AuthService() = default;
  ~AuthService() = default;

  drogon::Task<ResponseLoginDto>
  login(LoginDto body, const LoginDeviceInput& device) const;

  drogon::Task<ResponseLoginDto>
  registerUser(RegisterDto body, const LoginDeviceInput& device) const;

  drogon::Task<CreateDeviceLoginDto>
  createDeviceLogin(const LoginDeviceInput& device) const;

  drogon::Task<bool> approveDeviceLogin(const std::string& challengeId,
                                        int64_t approvingUserId) const;

  drogon::Task<DeviceLoginStatusDto>
  pollDeviceLogin(const std::string& challengeId) const;

  drogon::Task<ResponseRefreshTokenDto>
  refreshToken(const RefreshTokenInput& input) const;

  drogon::Task<void> logout(int64_t userId) const;

  drogon::Task<void> updateMe(int64_t userId,
                              const std::optional<std::string>& name) const;

private:
  drogon::Task<ResponseLoginDto>
  issueSession(const IssueSessionInput& input) const;

  // Issues the per-device secret in credential identity mode; empty in ip mode.
  drogon::Task<IssuedDeviceCredential>
  issueDeviceCredential(int64_t userId, const std::string& userAgent) const;

  JwtService jwtService_;
  PersonRepository personRepository_;
  UserRepository userRepository_;
  PrivatePortraitService privatePortraitService_;
  RefreshTokenRepository refreshTokenRepository_;
  DeviceCredentialRepository deviceCredentialRepository_;
  FaceEmbeddingRepository faceEmbeddingRepository_;
  UserInvitationRepository invitationRepository_;
  DeviceLoginChallengeRepository challengeRepository_;
  UserActionLogService userActionLogService_;
  SocketService socketService_;
  SyncAuditService syncAuditService_;
};
