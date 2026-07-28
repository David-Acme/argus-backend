#include "auth-service.hxx"

#include <ctime>
#include <drogon/drogon.h>
#include <shared/services/face/face-service.hxx>

drogon::Task<std::optional<ResponseLoginDto>>
AuthService::login(const LoginDto& body, const std::string& deviceHash,
                   const std::string& userAgent) const
{
  auto personId = FaceService::identify(body.image);
  if (!personId)
    co_return std::nullopt;

  auto person = co_await personRepository_.findById(*personId);
  if (!person || !person->userId)
    co_return std::nullopt;

  auto user = co_await userRepository_.findById(*person->userId);
  if (!user || !user->isActive)
    co_return std::nullopt;

  std::map<std::string, std::string> claims;
  claims["sub"] = std::to_string(user->id);

  auto accessToken = jwtService_.generateAccess(claims);
  auto refreshToken = jwtService_.generateRefresh(claims);

  RefreshTokenCreateInput rtInput;
  rtInput.userId = user->id;
  rtInput.accessToken = accessToken;
  rtInput.refreshToken = refreshToken;
  rtInput.deviceHash = deviceHash;
  rtInput.userAgent = userAgent;
  rtInput.expiresAt = static_cast<int64_t>(std::time(nullptr)) + 86400 * 30;

  co_await refreshTokenRepository_.create(rtInput);

  ResponseLoginDto result;
  result.accessToken = accessToken;
  result.refreshToken = refreshToken;
  result.userId = user->id;
  result.name = user->name + " " + user->lastName;
  result.role = user->role;
  result.personId = *personId;

  co_return result;
}

drogon::Task<std::optional<ResponseRefreshTokenDto>>
AuthService::refreshToken(const RefreshTokenDto& body,
                          const std::string& deviceHash) const
{
  auto claims = jwtService_.verifyRefresh(body.refreshToken);
  if (claims.empty())
    co_return std::nullopt;

  int64_t userId = 0;
  auto it = claims.find("sub");
  if (it != claims.end()) {
    try {
      userId = std::stoll(it->second);
    }
    catch (...) {
    }
  }
  if (userId == 0)
    co_return std::nullopt;

  auto existing =
      co_await refreshTokenRepository_.findByRefreshToken(userId,
                                                          body.refreshToken);
  if (!existing || !existing->isValid || existing->isUsed)
    co_return std::nullopt;

  co_await refreshTokenRepository_.markUsed(existing->id);

  std::map<std::string, std::string> newClaims;
  newClaims["sub"] = std::to_string(userId);

  auto accessToken = jwtService_.generateAccess(newClaims);
  auto refreshToken = jwtService_.generateRefresh(newClaims);

  RefreshTokenCreateInput rtInput;
  rtInput.userId = userId;
  rtInput.accessToken = accessToken;
  rtInput.refreshToken = refreshToken;
  rtInput.deviceHash = deviceHash;
  rtInput.userAgent = existing->userAgent;
  rtInput.expiresAt = static_cast<int64_t>(std::time(nullptr)) + 86400 * 30;
  co_await refreshTokenRepository_.create(rtInput);

  ResponseRefreshTokenDto result;
  result.accessToken = accessToken;
  result.refreshToken = refreshToken;

  co_return result;
}

drogon::Task<void> AuthService::logout(int64_t userId) const
{
  co_await refreshTokenRepository_.invalidateAllUser(userId);
  LOG_INFO << "AuthService: logged out user " << userId;
}
