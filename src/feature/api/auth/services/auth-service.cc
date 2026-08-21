#include "auth-service.hxx"

#include <config/app-config.hxx>
#include <ctime>
#include <drogon/drogon.h>
#include <iomanip>
#include <openssl/rand.h>
#include <sstream>
#include <shared/contracts/sync-operation.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/face/face-service.hxx>

namespace
{

// Login challenges live 2 minutes, long enough for a mobile to scan and
// approve while keeping the window of a stolen QR code short.
constexpr int64_t kDeviceLoginTtlSeconds = 120;

} // namespace

drogon::Task<ResponseLoginDto>
AuthService::login(LoginDto body, const LoginDeviceInput& device) const
{
  auto personId =
      co_await FaceService::instance().identifyAsync(std::move(body.image));
  if (!personId)
    throw ResponseException("Face not recognized", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);

  auto person = co_await personRepository_.findById(*personId);
  if (!person || !person->userId)
    throw ResponseException("Face not recognized", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);

  auto user = co_await userRepository_.findById(*person->userId);
  if (!user || !user->isActive)
    throw ResponseException("Face not recognized", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);

  co_return co_await issueSession(user->id, *personId, *user, device);
}

drogon::Task<ResponseLoginDto>
AuthService::registerUser(RegisterDto body,
                          const LoginDeviceInput& device) const
{
  if (!ConfigService::getBool("pairing.paired"))
    throw ResponseException("Server is not paired yet", 409,
                            AppConfig::ERROR_CODE_CONFLICT);

  auto face =
      co_await FaceService::instance().extractImageAsync(std::move(body.image));
  if (!face)
    throw ResponseException("Face not detected", 422,
                            AppConfig::ERROR_CODE_BAD_REQUEST);

  auto existing =
      FaceService::instance().faceDb().search(face->embedding.data());

  // A face that already belongs to an active user lets the caller straight in:
  // the owner gets an admin session, anyone else a session marked as
  // "already registered" so the client can inform them.
  if (existing && existing->second >= 0.80F) {
    auto person = co_await personRepository_.findById(existing->first);
    if (!person || !person->userId)
      throw ResponseException("Face already registered", 409,
                              AppConfig::ERROR_CODE_CONFLICT);

    auto user = co_await userRepository_.findById(*person->userId);
    if (!user || !user->isActive)
      throw ResponseException("Face not recognized", 401,
                              AppConfig::ERROR_CODE_UNAUTHORIZED);

    auto session =
        co_await issueSession(user->id, person->id, *user, device);
    session.alreadyRegistered = true;
    co_return session;
  }

  if (co_await userRepository_.hasOwner())
    throw ResponseException("An owner already exists", 409,
                            AppConfig::ERROR_CODE_CONFLICT);

  // Language: from the device on register; empty/invalid → system default.
  VoiceLang lang = voiceLangFromString(body.lang);
  if (lang == VoiceLang::System) {
    lang = voiceLangFromString(ConfigService::getString("stt.language"));
  }

  auto user =
      co_await userRepository_.create({.name = body.name.empty()
                                           ? "Administrador"
                                           : body.name,
                                       .lastName = "",
                                       .role = UserRole::Owner,
                                       .lang = voiceLangToString(lang)});

  auto person = co_await personRepository_.create(
      {.userId = user.id,
       .name = user.name,
       .alias = "",
       .observation = ""});

  auto embedding = std::string(
      reinterpret_cast<const char*>(face->embedding.data()),
      face->embedding.size() * sizeof(float));
  auto persisted = co_await faceEmbeddingRepository_.create(
      {.personId = person.id,
       .embedding = embedding,
       .angleLabel = "frontal",
       .quality = face->confidence});

  FaceService::instance().faceDb().insert(face->embedding.data(), person.id,
                                          persisted.id);

  co_return co_await issueSession(user.id, person.id, user, device);
}

drogon::Task<HasAdminResult> AuthService::hasAdmin() const
{
  co_return HasAdminResult{
      .paired = ConfigService::getBool("pairing.paired"),
      .hasAdmin = co_await userRepository_.hasOwner(),
  };
}

drogon::Task<CreateDeviceLoginDto>
AuthService::createDeviceLogin(const LoginDeviceInput& device) const
{
  unsigned char buf[32]{};
  if (RAND_bytes(buf, sizeof(buf)) != 1)
    throw ResponseException("Failed to generate login challenge", 500,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);

  std::ostringstream hex;
  for (unsigned char b : buf)
    hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);

  const int64_t now = std::time(nullptr);
  const auto challengeId = hex.str();
  co_await challengeRepository_.create({.challengeId = challengeId,
                                        .deviceHash = device.deviceHash,
                                        .userAgent = device.userAgent,
                                        .expiresAt = now +
                                                     kDeviceLoginTtlSeconds});

  CreateDeviceLoginDto dto;
  dto.challengeId = challengeId;
  dto.expiresAt = now + kDeviceLoginTtlSeconds;
  co_return dto;
}

drogon::Task<bool>
AuthService::approveDeviceLogin(const std::string& challengeId,
                                int64_t approvingUserId) const
{
  auto challenge = co_await challengeRepository_.findByChallengeId(challengeId);
  if (!challenge || challenge->status != "pending")
    throw ResponseException("Challenge not found", 404,
                            AppConfig::ERROR_CODE_NOT_FOUND);
  if (challenge->expiresAt <= std::time(nullptr)) {
    co_await challengeRepository_.remove(challengeId);
    throw ResponseException("Challenge expired", 404,
                            AppConfig::ERROR_CODE_NOT_FOUND);
  }

  auto user = co_await userRepository_.findById(approvingUserId);
  if (!user || !user->isActive)
    throw ResponseException("Face not recognized", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);

  std::map<std::string, std::string> claims;
  claims["sub"] = std::to_string(approvingUserId);
  const auto accessToken = jwtService_.generateAccess(claims);
  const auto refreshToken = jwtService_.generateRefresh(claims);

  // Bind the session to the DESKTOP device hash captured when the challenge
  // was created, so the desktop's own DeviceFilter matches when it polls.
  RefreshTokenCreateInput rtInput;
  rtInput.userId = approvingUserId;
  rtInput.accessToken = accessToken;
  rtInput.refreshToken = refreshToken;
  rtInput.deviceHash = challenge->deviceHash;
  rtInput.userAgent = challenge->userAgent;
  rtInput.expiresAt = std::time(nullptr) + jwtService_.refreshTtlSeconds();
  co_await refreshTokenRepository_.create(rtInput);

  co_await challengeRepository_.markApproved(challengeId, approvingUserId,
                                             accessToken, refreshToken);
  co_return true;
}

drogon::Task<DeviceLoginStatusDto>
AuthService::pollDeviceLogin(const std::string& challengeId) const
{
  auto challenge = co_await challengeRepository_.findByChallengeId(challengeId);
  if (!challenge)
    co_return DeviceLoginStatusDto{.status = "expired"};

  if (challenge->status == "approved") {
    DeviceLoginStatusDto dto;
    dto.status = "approved";
    dto.accessToken = challenge->accessToken;
    dto.refreshToken = challenge->refreshToken;
    if (challenge->userId) {
      auto user = co_await userRepository_.findById(*challenge->userId);
      if (user) {
        dto.userId = user->id;
        dto.name = user->name + " " + user->lastName;
        dto.role = user->role;
      }
    }
    // Single-use delivery: the tokens are handed over exactly once.
    co_await challengeRepository_.remove(challengeId);
    co_return dto;
  }

  if (challenge->expiresAt <= std::time(nullptr)) {
    co_await challengeRepository_.remove(challengeId);
    co_return DeviceLoginStatusDto{.status = "expired"};
  }

  co_return DeviceLoginStatusDto{.status = "pending"};
}

drogon::Task<ResponseRefreshTokenDto>
AuthService::refreshToken(const RefreshTokenDto& body,
                          const std::string& deviceHash,
                          const std::string& userAgent) const
{
  auto claims = jwtService_.verifyRefresh(body.refreshToken);
  if (claims.empty())
    throw ResponseException("Invalid or expired refresh token", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);

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
    throw ResponseException("Invalid or expired refresh token", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);

  auto existing =
      co_await refreshTokenRepository_.findByRefreshToken(userId,
                                                          body.refreshToken);
  if (!existing || !existing->isValid || existing->isUsed)
    throw ResponseException("Invalid or expired refresh token", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);

  if (existing->expiresAt <= std::time(nullptr)) {
    LOG_WARN << "AuthService: expired refresh token for user " << userId;
    throw ResponseException("Invalid or expired refresh token", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);
  }

  if (!existing->userAgent.empty() && !userAgent.empty() &&
      existing->userAgent != userAgent) {
    LOG_WARN << "AuthService: user agent mismatch on refresh for user " << userId;
    throw ResponseException("Invalid or expired refresh token", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);
  }

  co_await refreshTokenRepository_.markUsed(existing->id);
  // Used and expired rows pile up otherwise: one login per day leaves a year of
  // dead tokens behind.
  co_await refreshTokenRepository_.pruneStale(userId);

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
  rtInput.expiresAt = static_cast<int64_t>(std::time(nullptr)) +
                      jwtService_.refreshTtlSeconds();
  co_await refreshTokenRepository_.create(rtInput);

  ResponseRefreshTokenDto result;
  result.accessToken = accessToken;
  result.refreshToken = refreshToken;

  co_return result;
}

drogon::Task<void> AuthService::logout(int64_t userId) const
{
  co_await refreshTokenRepository_.invalidateAllUser(userId);

  co_await userActionLogService_.record({.userId = userId,
                                         .recordId = userId,
                                         .tableName = TableName::User,
                                         .action = UserAction::Delete,
                                         .oldData = Json::Value(),
                                         .newData = Json::Value(),
                                         .ipAddress = ""});

  LOG_INFO << "AuthService: logged out user " << userId;
}

drogon::Task<void>
AuthService::updateMe(int64_t userId,
                      const std::optional<std::string>& name) const
{
  auto user = co_await userRepository_.update(
      userId, {.name = name, .lastName = std::nullopt, .role = std::nullopt,
               .isActive = std::nullopt});

  SocketEmitDto emit;
  emit.operation = SyncOperation::Add;
  emit.option = TableName::User;
  emit.obj = user.toJson();
  socketService_.emitModule(TableName::User, emit);
}

drogon::Task<ResponseLoginDto>
AuthService::issueSession(int64_t userId, int64_t personId,
                          const UserSchema& user,
                          const LoginDeviceInput& device) const
{
  std::map<std::string, std::string> claims;
  claims["sub"] = std::to_string(userId);

  auto accessToken = jwtService_.generateAccess(claims);
  auto refreshToken = jwtService_.generateRefresh(claims);

  RefreshTokenCreateInput rtInput;
  rtInput.userId = userId;
  rtInput.accessToken = accessToken;
  rtInput.refreshToken = refreshToken;
  rtInput.deviceHash = device.deviceHash;
  rtInput.userAgent = device.userAgent;
  rtInput.expiresAt = static_cast<int64_t>(std::time(nullptr)) +
                      jwtService_.refreshTtlSeconds();

  co_await refreshTokenRepository_.create(rtInput);

  ResponseLoginDto result;
  result.accessToken = accessToken;
  result.refreshToken = refreshToken;
  result.userId = userId;
  result.name = user.name + " " + user.lastName;
  result.role = user.role;
  result.personId = personId;

  Json::Value session;
  session["deviceHash"] = device.deviceHash;
  session["userAgent"] = device.userAgent;

  co_await userActionLogService_.record({.userId = userId,
                                         .recordId = userId,
                                         .tableName = TableName::User,
                                         .action = UserAction::Create,
                                         .oldData = Json::Value(),
                                         .newData = session,
                                         .ipAddress = ""});

  co_return result;
}
