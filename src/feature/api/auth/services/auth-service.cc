#include "auth-service.hxx"

#include <config/app-config.hxx>
#include <ctime>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <feature/api/invitation/services/invitation-feature-service.hxx>
#include <filter/device/device-filter.hxx>
#include <future>
#include <iomanip>
#include <map>
#include <mutex>
#include <openssl/rand.h>
#include <sstream>
#include <string_view>
#include <shared/contracts/identity-change-sink.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>

namespace user_enrollment_query
{
inline constexpr std::string_view COUNT_USERS =
    "SELECT COUNT(*) FROM user WHERE deleted_at IS NULL";
inline constexpr std::string_view INSERT_USER =
    "INSERT INTO user (name, last_name, role, lang, is_active) "
    "VALUES (?, ?, ?, ?, 1)";
inline constexpr std::string_view INSERT_PERSON =
    "INSERT INTO person (user_id, name, alias, observation, first_seen_at, "
    "last_seen_at) VALUES (?, ?, '', '', strftime('%s', 'now'), "
    "strftime('%s', 'now'))";
inline constexpr std::string_view INSERT_FACE_EMBEDDING =
    "INSERT INTO face_embedding (person_id, embedding, angle_label, quality) "
    "VALUES (?, ?, 'frontal', ?)";
inline constexpr std::string_view TRY_CONSUME =
    "UPDATE user_invitation SET redemption_count = redemption_count + 1, "
    "updated_at = strftime('%s', 'now') WHERE token_hash = ? "
    "AND revoked_at IS NULL AND expires_at > ? "
    "AND redemption_count < max_redemptions";
inline constexpr std::string_view INSERT_REDEMPTION =
    "INSERT INTO invitation_redemption (invitation_id, user_id) VALUES (?, ?)";
} // namespace user_enrollment_query

namespace
{

// Login challenges live 2 minutes, long enough for a mobile to scan and
// approve while keeping the window of a stolen QR code short.
constexpr int64_t kDeviceLoginTtlSeconds = 120;

struct PendingDeviceSecret
{
  std::string secret;
  int64_t expiresAt{0};
};

// A desktop challenge approved in credential mode delivers its issued device
// secret exactly once to the polling device: held in memory only, TTL bound
// to the challenge, never persisted.
std::map<std::string, PendingDeviceSecret>& pendingDeviceSecrets()
{
  static std::map<std::string, PendingDeviceSecret> secrets;
  return secrets;
}

std::mutex& pendingDeviceSecretsMutex()
{
  static std::mutex mutex;
  return mutex;
}

void storePendingDeviceSecret(const std::string& challengeId,
                              const std::string& secret, int64_t expiresAt)
{
  std::lock_guard lock(pendingDeviceSecretsMutex());
  const int64_t now = std::time(nullptr);
  std::erase_if(pendingDeviceSecrets(), [&](const auto& entry) {
    return entry.second.expiresAt <= now;
  });
  pendingDeviceSecrets()[challengeId] = {secret, expiresAt};
}

std::string takePendingDeviceSecret(const std::string& challengeId)
{
  std::lock_guard lock(pendingDeviceSecretsMutex());
  const auto it = pendingDeviceSecrets().find(challengeId);
  if (it == pendingDeviceSecrets().end())
    return {};
  auto secret = std::move(it->second.secret);
  pendingDeviceSecrets().erase(it);
  return secret;
}

} // namespace

drogon::Task<ResponseLoginDto>
AuthService::login(LoginDto body, const LoginDeviceInput& device) const
{
  auto personId =
      co_await FaceService::instance().identifyAsync(std::move(body.image));
  if (!personId) {
    throw ResponseException("Face not recognized", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);
  }

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

  const auto portraitImage = body.image;
  auto face =
      co_await FaceService::instance().extractImageAsync(std::move(body.image));
  if (!face) {
    throw ResponseException("Face could not be extracted", 422,
                            AppConfig::ERROR_CODE_BAD_REQUEST);
  }

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

  // Language: from the device on register; empty/invalid → system default.
  VoiceLang lang = voiceLangFromString(body.lang);
  if (lang == VoiceLang::System) {
    lang = voiceLangFromString(ConfigService::getString("stt.language"));
  }

  const bool isInitialOwner = !co_await userRepository_.hasAnyUser();
  std::optional<UserInvitationSchema> invitation;
  std::string invitationHash;
  UserRole role = UserRole::Owner;
  if (!isInitialOwner) {
    if (body.inviteCode.empty())
      throw ResponseException("A valid invitation is required", 403,
                              AppConfig::ERROR_CODE_FORBIDDEN);
    invitationHash = InvitationFeatureService::hashToken(body.inviteCode);
    invitation = co_await invitationRepository_.findByTokenHash(invitationHash);
    const int64_t now = std::time(nullptr);
    if (!invitation || invitation->revokedAt || invitation->expiresAt <= now ||
        invitation->redemptionCount >= invitation->maxRedemptions) {
      throw ResponseException("Invitation is invalid or expired", 404,
                              AppConfig::ERROR_CODE_NOT_FOUND);
    }
    role = invitation->role;
  }

  const auto name = body.name.empty()
                        ? (isInitialOwner ? "Administrador" : "Usuario")
                        : body.name;
  const auto embedding = std::string(
      reinterpret_cast<const char*>(face->embedding.data()),
      face->embedding.size() * sizeof(float));
  const int64_t now = std::time(nullptr);
  int64_t userId = 0;
  int64_t personId = 0;
  int64_t faceEmbeddingId = 0;
  struct FaceIndexInput
  {
    std::vector<float> embedding;
    int64_t personId{0};
    int64_t faceEmbeddingId{0};
  };
  auto indexInput = std::make_shared<FaceIndexInput>(FaceIndexInput{
      .embedding = face->embedding,
  });
  auto indexResult = std::make_shared<std::promise<bool>>();
  auto indexFuture =
      std::make_shared<std::future<bool>>(indexResult->get_future());

  {
    auto transaction = co_await DbService::client()->newTransactionCoro(
        drogon::orm::TransactionType::Immediate);
    transaction->setCommitCallback(
        [indexResult, indexInput](bool committed) {
          if (!committed) {
            indexResult->set_value(false);
            return;
          }
          indexResult->set_value(FaceService::instance().faceDb().insert(
              indexInput->embedding.data(), indexInput->personId,
              indexInput->faceEmbeddingId));
        });
    const auto userCount =
        co_await transaction->execSqlCoro(user_enrollment_query::COUNT_USERS.data());

    if (isInitialOwner && !userCount.empty() &&
        userCount.front()[0].as<int64_t>() > 0) {
      transaction->rollback();
      throw ResponseException("An owner already exists", 409,
                              AppConfig::ERROR_CODE_CONFLICT);
    }
    if (!isInitialOwner) {
      const auto consumed = co_await transaction->execSqlCoro(
          user_enrollment_query::TRY_CONSUME.data(), invitationHash, now);
      if (consumed.affectedRows() != 1) {
        transaction->rollback();
        throw ResponseException("Invitation is invalid or expired", 404,
                                AppConfig::ERROR_CODE_NOT_FOUND);
      }
    }

    const auto userResult = co_await transaction->execSqlCoro(
        user_enrollment_query::INSERT_USER.data(), name, "",
        userRoleToString(role), voiceLangToString(lang));
    userId = userResult.insertId();

    const auto personResult = co_await transaction->execSqlCoro(
        user_enrollment_query::INSERT_PERSON.data(), userId, name);
    personId = personResult.insertId();
    indexInput->personId = personId;

    const auto embeddingResult = co_await transaction->execSqlCoro(
        user_enrollment_query::INSERT_FACE_EMBEDDING.data(), personId,
        embedding, face->confidence);
    faceEmbeddingId = embeddingResult.insertId();
    indexInput->faceEmbeddingId = faceEmbeddingId;

    if (invitation) {
      co_await transaction->execSqlCoro(
          user_enrollment_query::INSERT_REDEMPTION.data(), invitation->id,
          userId);
    }
  }

  const bool indexed = co_await BlockingTask<bool>(
      [indexFuture] { return indexFuture->get(); });
  if (!indexed)
    throw ResponseException("Could not index enrolled face", 503,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);

  co_await privatePortraitService_.store(userId, portraitImage);

  // Memory catalog replica feed (Ruling BX): the enrolled person row fans
  // out on the identity change subject.
  if (identity_change::getSink()) {
    Json::Value row(Json::objectValue);
    row["id"] = static_cast<Json::Int64>(personId);
    row["user_id"] = static_cast<Json::Int64>(userId);
    row["name"] = name;
    row["alias"] = "";
    identity_change::getSink()->publish(
        {.table = "person", .id = personId, .deleted = false, .row = row});
  }

  UserSchema user;
  user.id = userId;
  user.name = name;
  user.lastName = "";
  user.role = role;
  user.lang = voiceLangToString(lang);
  user.isActive = true;
  user.createdAt = now;

  SocketEmitDto emit;
  emit.operation = SyncOperation::Add;
  emit.option = TableName::User;
  emit.obj = user.toJson();
  socketService_.emitModule(TableName::User, emit);

  if (invitation) {
    const auto consumedInvitation =
        co_await invitationRepository_.findById(invitation->id);
    if (consumedInvitation) {
      co_await syncAuditService_.publishModule({
          .recordId = consumedInvitation->id,
          .tableName = TableName::UserInvitation,
          .before = invitation->toJson(),
          .after = consumedInvitation->toJson(),
          .actorId = user.id,
      });

      Json::Value enrollment(Json::objectValue);
      enrollment["event"] = "invitation_enrollment";
      enrollment["invitationId"] = invitation->id;
      enrollment["userId"] = userId;
      co_await userActionLogService_.record({
          .userId = userId,
          .recordId = invitation->id,
          .tableName = TableName::UserInvitation,
          .action = UserAction::Create,
          .oldData = Json::Value(),
          .newData = enrollment,
          .ipAddress = "",
      });
    }
  }

  co_return co_await issueSession(userId, personId, user, device);
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

  const auto credential = co_await issueDeviceCredential(
      approvingUserId, challenge->userAgent);

  // Bind the session to the DESKTOP device hash captured when the challenge
  // was created, so the desktop's own DeviceFilter matches when it polls. In
  // credential mode the freshly issued credential defines that hash instead.
  RefreshTokenCreateInput rtInput;
  rtInput.userId = approvingUserId;
  rtInput.accessToken = accessToken;
  rtInput.refreshToken = refreshToken;
  rtInput.deviceHash = credential.deviceHash.empty()
                           ? challenge->deviceHash
                           : credential.deviceHash;
  rtInput.userAgent = challenge->userAgent;
  rtInput.expiresAt = std::time(nullptr) + jwtService_.refreshTtlSeconds();
  co_await refreshTokenRepository_.create(rtInput);

  co_await challengeRepository_.markApproved(challengeId, approvingUserId,
                                             accessToken, refreshToken);
  if (!credential.secret.empty())
    storePendingDeviceSecret(challengeId, credential.secret,
                             challenge->expiresAt);
  co_return true;
}

drogon::Task<DeviceLoginStatusDto>
AuthService::pollDeviceLogin(const std::string& challengeId) const
{
  auto challenge = co_await challengeRepository_.findByChallengeId(challengeId);
  if (!challenge)
    co_return DeviceLoginStatusDto{.status = "expired",
                                   .accessToken = "",
                                   .refreshToken = "",
                                   .userId = 0,
                                   .name = "",
                                   .deviceSecret = "",
                                   .role = UserRole::Guest};

  if (challenge->status == "approved") {
    DeviceLoginStatusDto dto;
    dto.status = "approved";
    dto.accessToken = challenge->accessToken;
    dto.refreshToken = challenge->refreshToken;
    dto.deviceSecret = takePendingDeviceSecret(challengeId);
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
    co_return DeviceLoginStatusDto{.status = "expired",
                                   .accessToken = "",
                                   .refreshToken = "",
                                   .userId = 0,
                                   .name = "",
                                   .deviceSecret = "",
                                   .role = UserRole::Guest};
  }

  co_return DeviceLoginStatusDto{.status = "pending",
                                 .accessToken = "",
                                 .refreshToken = "",
                                 .userId = 0,
                                 .name = "",
                                 .deviceSecret = "",
                                 .role = UserRole::Guest};
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

  if (!co_await refreshTokenRepository_.markUsed(existing->id))
    throw ResponseException("Invalid or expired refresh token", 401,
                            AppConfig::ERROR_CODE_UNAUTHORIZED);
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

  const auto user = co_await userRepository_.findById(userId);
  SocketEmitDto context;
  context.operation = SyncOperation::AuthContextChanged;
  context.option = TableName::User;
  context.obj = user ? user->toJson() : Json::Value(Json::objectValue);
  context.obj["id"] = userId;
  context.obj["isActive"] = false;
  context.obj["resync"] = false;
  socketService_.disconnectUser(userId, context);

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
  const auto before = co_await userRepository_.findById(userId);
  if (!before)
    throw ResponseException("User not found", 404, AppConfig::ERROR_CODE_NOT_FOUND);
  auto user = co_await userRepository_.update(
      userId, {.name = name, .lastName = std::nullopt, .role = std::nullopt,
               .isActive = std::nullopt});
  auto users = co_await userRepository_.findAll();
  std::vector<int64_t> recipients{user.id};
  for (const auto& recipient : users) {
    if (recipient.role == UserRole::Owner || recipient.role == UserRole::Guard)
      recipients.push_back(recipient.id);
  }
  co_await syncAuditService_.publishUsers({
      .recordId = user.id,
      .tableName = TableName::User,
      .before = before->toJson(),
      .after = user.toJson(),
      .userIds = std::move(recipients),
  });
}

drogon::Task<IssuedDeviceCredential>
AuthService::issueDeviceCredential(int64_t userId,
                                   const std::string& userAgent) const
{
  IssuedDeviceCredential issued;
  if (ConfigService::getString("device.identity_mode") != "credential")
    co_return issued;

  unsigned char buf[32]{};
  if (RAND_bytes(buf, sizeof(buf)) != 1)
    throw ResponseException("Failed to issue device credential", 500,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);

  std::ostringstream hex;
  for (unsigned char b : buf)
    hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  issued.secret = hex.str();

  const auto secretHash = DeviceFilter::sha256Hex(issued.secret);
  issued.deviceHash =
      DeviceFilter::credentialFingerprint(userAgent, secretHash);
  co_await deviceCredentialRepository_.create({.userId = userId,
                                               .deviceHash = issued.deviceHash,
                                               .secretHash = secretHash});
  co_return issued;
}

drogon::Task<ResponseLoginDto>
AuthService::issueSession(int64_t userId, int64_t personId,
                          const UserSchema& user,
                          const LoginDeviceInput& device) const
{
  const auto credential =
      co_await issueDeviceCredential(userId, device.userAgent);
  const std::string deviceHash =
      credential.deviceHash.empty() ? device.deviceHash : credential.deviceHash;

  std::map<std::string, std::string> claims;
  claims["sub"] = std::to_string(userId);

  auto accessToken = jwtService_.generateAccess(claims);
  auto refreshToken = jwtService_.generateRefresh(claims);

  RefreshTokenCreateInput rtInput;
  rtInput.userId = userId;
  rtInput.accessToken = accessToken;
  rtInput.refreshToken = refreshToken;
  rtInput.deviceHash = deviceHash;
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
  result.deviceSecret = credential.secret;

  Json::Value session;
  session["deviceHash"] = deviceHash;
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
