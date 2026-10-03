#include "auth-feature-service.hxx"

#include <array>
#include <auth/auth-errors.hxx>
#include <auth/device-filter.hxx>
#include <auth/device-login-status.hxx>
#include <charconv>
#include <ctime>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <identity/identity-client.hxx>
#include <map>
#include <mutex>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <optional>
#include <runtime/blocking-task.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <string>
#include <string_view>
#include <sync/auth-change-sink.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-control-sink.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <sync/user-action.hxx>
#include <trantor/utils/Logger.h>

namespace
{

constexpr int64_t kDeviceLoginTtlSeconds = 120;

struct ExpiringSecretInput
{
  std::string challengeId;
  std::string secret;
  int64_t expiresAt{0};
};

class ExpiringSecrets
{
public:
  void store(const ExpiringSecretInput& input)
  {
    std::scoped_lock lock(mutex_);
    const int64_t now = std::time(nullptr);
    std::erase_if(secrets_, [now](const auto& entry) { return entry.second.expiresAt <= now; });
    secrets_[input.challengeId] = {.secret = input.secret, .expiresAt = input.expiresAt};
  }

  std::string take(const std::string& challengeId)
  {
    std::scoped_lock lock(mutex_);
    auto node = secrets_.extract(challengeId);
    return node.empty() ? std::string{} : std::move(node.mapped().secret);
  }

  std::optional<std::string> peek(const std::string& challengeId)
  {
    std::scoped_lock lock(mutex_);
    const auto it = secrets_.find(challengeId);
    if (it == secrets_.end() || it->second.expiresAt <= std::time(nullptr))
      return std::nullopt;
    return it->second.secret;
  }

private:
  struct Entry
  {
    std::string secret;
    int64_t expiresAt{0};
  };

  std::mutex mutex_;
  std::map<std::string, Entry> secrets_;
};

ExpiringSecrets& pendingDeviceSecrets()
{
  static ExpiringSecrets secrets;
  return secrets;
}

ExpiringSecrets& pendingPollHashes()
{
  static ExpiringSecrets hashes;
  return hashes;
}

bool sameDigest(std::string_view left, std::string_view right)
{
  return left.size() == right.size() &&
         CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

bool pollerOwnsChallenge(const DeviceLoginPollInput& input,
                         const DeviceLoginChallengeSchema& challenge)
{
  if (const auto pollHash = pendingPollHashes().peek(input.challengeId))
    return !input.proof.empty() &&
           sameDigest(DeviceFilter::sha256Hex(input.proof), *pollHash);
  return challenge.deviceHash == input.device.deviceHash;
}

std::string randomSecret()
{
  std::array<unsigned char, 32> buffer{};
  if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1)
    return {};

  constexpr std::string_view kHexDigits = "0123456789abcdef";
  std::string secret;
  secret.reserve(buffer.size() * 2);
  for (const unsigned char byte : buffer) {
    secret.push_back(kHexDigits[byte >> 4]);
    secret.push_back(kHexDigits[byte & 0x0f]);
  }
  return secret;
}

std::optional<int64_t> userIdOfClaims(
    const std::map<std::string, std::string>& claims)
{
  const auto it = claims.find("sub");
  if (it == claims.end())
    return std::nullopt;

  const std::string& sub = it->second;
  int64_t userId = 0;
  const auto parsed =
      std::from_chars(sub.data(), sub.data() + sub.size(), userId);
  if (parsed.ec != std::errc{} || parsed.ptr != sub.data() + sub.size() ||
      userId <= 0)
    return std::nullopt;
  return userId;
}

SessionUser sessionUserOf(const argus::identity::v1::UserIdentity& user)
{
  SessionUser result;
  result.userId = user.user_id();
  result.name = user.name();
  result.lastName = user.last_name();
  result.role = userRoleFromString(user.role());
  return result;
}

DeviceLoginStatusDto idleDeviceLogin(DeviceLoginStatus status)
{
  DeviceLoginStatusDto result;
  result.status = status;
  return result;
}

drogon::Task<std::optional<argus::identity::v1::IdentifyPersonResponse>>
identifyPerson(const IdentityClient* client, std::string image)
{
  if (client == nullptr)
    co_return std::nullopt;
  co_return co_await BlockingTask<
      std::optional<argus::identity::v1::IdentifyPersonResponse>>(
      [client, image = std::move(image)]() { return client->identifyPerson(image); });
}

drogon::Task<std::optional<argus::identity::v1::RegisterUserResponse>>
registerIdentityUser(const IdentityClient* client, RegisterUserInput input)
{
  if (client == nullptr)
    co_return std::nullopt;
  co_return co_await BlockingTask<
      std::optional<argus::identity::v1::RegisterUserResponse>>(
      [client, request = std::move(input)]() { return client->registerUser(request); });
}

drogon::Task<std::optional<argus::identity::v1::GetUserResponse>>
fetchIdentityUser(const IdentityClient* client, int64_t userId)
{
  if (client == nullptr)
    co_return std::nullopt;
  co_return co_await BlockingTask<
      std::optional<argus::identity::v1::GetUserResponse>>(
      [client, userId]() { return client->getUser(userId); });
}

drogon::Task<std::optional<argus::identity::v1::UserIdentity>>
renameIdentityUser(const IdentityClient* client,
                   const UpdateUserNameInput& input)
{
  if (client == nullptr)
    co_return std::nullopt;
  co_return co_await BlockingTask<
      std::optional<argus::identity::v1::UserIdentity>>(
      [client, input]() { return client->updateUserName(input); });
}

}

AuthFeatureService::AuthFeatureService(Dependencies dependencies)
    : dependencies_(std::move(dependencies))
{
}

drogon::Task<ResponseLoginDto>
AuthFeatureService::login(LoginDto body, const LoginDeviceInput& device) const
{
  const auto answer =
      co_await identifyPerson(dependencies_.identity, std::move(body.image));
  if (!answer || !answer->matched() || !answer->has_user_id() ||
      answer->user_id() <= 0)
    throw ResponseException(AuthErrors::FaceNotRecognized);

  SessionUser user;
  user.userId = answer->user_id();
  user.name = answer->name();
  user.lastName = answer->last_name();
  user.role = userRoleFromString(answer->role());

  co_return co_await issueSession(
      {.user = std::move(user),
       .personId = answer->person_id(),
       .device = device});
}

drogon::Task<ResponseLoginDto>
AuthFeatureService::registerUser(RegisterDto body,
                                 const LoginDeviceInput& device) const
{
  const auto answer = co_await registerIdentityUser(
      dependencies_.identity,
      {.image = std::move(body.image),
       .name = body.name,
       .invitationToken = body.inviteCode,
       .lang = body.lang,
       .deviceHash = device.deviceHash});
  if (!answer)
    throw ResponseException(AuthErrors::IdentityUnavailable);

  switch (answer->outcome()) {
    case argus::identity::v1::REGISTER_USER_ENROLLED:
    case argus::identity::v1::REGISTER_USER_ALREADY_REGISTERED:
      break;
    case argus::identity::v1::REGISTER_USER_NOT_PAIRED:
      throw ResponseException(AuthErrors::ServerNotPaired);
    case argus::identity::v1::REGISTER_USER_FACE_EXTRACTION_FAILED:
      throw ResponseException(AuthErrors::FaceExtractionFailed);
    case argus::identity::v1::REGISTER_USER_FACE_ALREADY_REGISTERED:
      throw ResponseException(AuthErrors::FaceAlreadyRegistered);
    case argus::identity::v1::REGISTER_USER_FACE_NOT_RECOGNIZED:
      throw ResponseException(AuthErrors::FaceNotRecognized);
    case argus::identity::v1::REGISTER_USER_INVITATION_REQUIRED:
      throw ResponseException(AuthErrors::InvitationRequired);
    case argus::identity::v1::REGISTER_USER_INVITATION_INVALID:
      throw ResponseException(AuthErrors::InvitationInvalidOrExpired);
    case argus::identity::v1::REGISTER_USER_OWNER_ALREADY_EXISTS:
      throw ResponseException(AuthErrors::OwnerAlreadyExists);
    case argus::identity::v1::REGISTER_USER_FACE_INDEX_FAILED:
      throw ResponseException(AuthErrors::EnrolledFaceIndexFailed);
    default:
      throw ResponseException(AuthErrors::IdentityUnavailable);
  }

  if (!answer->has_user() || answer->user().user_id() <= 0)
    throw ResponseException(AuthErrors::IdentityUnavailable);

  auto session = co_await issueSession({.user = sessionUserOf(answer->user()),
                                        .personId = answer->person_id(),
                                        .device = device});
  session.alreadyRegistered =
      answer->outcome() ==
      argus::identity::v1::REGISTER_USER_ALREADY_REGISTERED;
  co_return session;
}

drogon::Task<CreateDeviceLoginDto>
AuthFeatureService::createDeviceLogin(const DeviceLoginStartInput& input) const
{
  const std::string challengeId = randomSecret();
  if (challengeId.empty())
    throw ResponseException(AuthErrors::LoginChallengeGenerationFailed);

  const auto now = static_cast<int64_t>(std::time(nullptr));
  co_await dependencies_.challengeRepository.removeExpired(now);
  const int64_t expiresAt = now + kDeviceLoginTtlSeconds;
  co_await dependencies_.challengeRepository.create(
      {.challengeId = challengeId,
       .deviceHash = input.device.deviceHash,
       .userAgent = input.device.userAgent,
       .expiresAt = expiresAt});
  if (!input.pollHash.empty())
    pendingPollHashes().store({.challengeId = challengeId,
                               .secret = input.pollHash,
                               .expiresAt = expiresAt});

  co_return CreateDeviceLoginDto{.challengeId = challengeId,
                                 .expiresAt = expiresAt};
}

drogon::Task<void>
AuthFeatureService::approveDeviceLogin(const std::string& challengeId,
                                       int64_t approvingUserId) const
{
  const auto challenge =
      co_await dependencies_.challengeRepository.findByChallengeId(challengeId);
  if (!challenge || challenge->status != DeviceLoginStatus::Pending)
    throw ResponseException(AuthErrors::ChallengeNotFound);
  if (challenge->expiresAt <= static_cast<int64_t>(std::time(nullptr))) {
    co_await dependencies_.challengeRepository.remove(challengeId);
    throw ResponseException(AuthErrors::ChallengeExpired);
  }

  const auto answer =
      co_await fetchIdentityUser(dependencies_.identity, approvingUserId);
  if (!answer)
    throw ResponseException(AuthErrors::IdentityUnavailable);
  if (!answer->has_user() || !answer->user().is_active())
    throw ResponseException(AuthErrors::AccessDenied);

  std::map<std::string, std::string> claims;
  claims["sub"] = std::to_string(approvingUserId);

  IssuedDeviceCredential credential;
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    credential = co_await issueDeviceCredential(
        {.userId = approvingUserId,
         .userAgent = challenge->userAgent,
         .client = transaction.get()});

    const std::string accessToken =
        dependencies_.jwtService.generateAccess(claims);
    const std::string refreshToken =
        dependencies_.jwtService.generateRefresh(claims);

    co_await dependencies_.refreshTokenRepository.create(
        {.userId = approvingUserId,
         .accessToken = accessToken,
         .refreshToken = refreshToken,
         .deviceHash = credential.deviceHash.empty() ? challenge->deviceHash
                                                     : credential.deviceHash,
         .userAgent = challenge->userAgent,
         .expiresAt = static_cast<int64_t>(std::time(nullptr)) +
                      dependencies_.jwtService.refreshTtlSeconds(),
         .client = transaction.get()});

    const bool approved =
        co_await dependencies_.challengeRepository.markApproved(
            {.challengeId = challengeId,
             .userId = approvingUserId,
             .accessToken = accessToken,
             .refreshToken = refreshToken,
             .client = transaction.get()});
    if (!approved)
      throw ResponseException(AuthErrors::ChallengeNotFound);

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(AuthErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  if (!credential.secret.empty())
    pendingDeviceSecrets().store({.challengeId = challengeId,
                              .secret = credential.secret,
                              .expiresAt = challenge->expiresAt});
}

drogon::Task<DeviceLoginStatusDto>
AuthFeatureService::pollDeviceLogin(const DeviceLoginPollInput& input) const
{
  const std::string& challengeId = input.challengeId;
  const auto challenge =
      co_await dependencies_.challengeRepository.findByChallengeId(challengeId);
  if (!challenge)
    co_return idleDeviceLogin(DeviceLoginStatus::Expired);

  if (challenge->expiresAt <= static_cast<int64_t>(std::time(nullptr))) {
    co_await dependencies_.challengeRepository.remove(challengeId);
    co_return idleDeviceLogin(DeviceLoginStatus::Expired);
  }
  if (challenge->status == DeviceLoginStatus::Expired)
    co_return idleDeviceLogin(DeviceLoginStatus::Expired);
  if (challenge->status != DeviceLoginStatus::Approved ||
      !pollerOwnsChallenge(input, *challenge))
    co_return idleDeviceLogin(DeviceLoginStatus::Pending);
  if (!co_await dependencies_.challengeRepository.claimApproved(
          challengeId, static_cast<int64_t>(std::time(nullptr))))
    co_return idleDeviceLogin(DeviceLoginStatus::Expired);

  DeviceLoginStatusDto result;
  result.status = DeviceLoginStatus::Approved;
  result.accessToken = challenge->accessToken;
  result.refreshToken = challenge->refreshToken;
  result.deviceSecret = pendingDeviceSecrets().take(challengeId);
  static_cast<void>(pendingPollHashes().take(challengeId));
  if (challenge->userId) {
    const auto answer =
        co_await fetchIdentityUser(dependencies_.identity, *challenge->userId);
    if (answer && answer->has_user()) {
      result.userId = answer->user().user_id();
      result.name = answer->user().name() + " " + answer->user().last_name();
      result.role = userRoleFromString(answer->user().role());
    }
  }
  co_await dependencies_.challengeRepository.remove(challengeId);
  co_return result;
}

drogon::Task<ResponseRefreshTokenDto>
AuthFeatureService::refreshToken(const RefreshTokenInput& input) const
{
  const auto claims =
      dependencies_.jwtService.verifyRefresh(input.body.refreshToken);
  const auto userId = userIdOfClaims(claims);
  if (!userId)
    throw ResponseException(AuthErrors::RefreshTokenInvalidOrExpired);

  const auto existing = co_await dependencies_.refreshTokenRepository
                            .findByRefreshToken(*userId, input.body.refreshToken);
  if (!existing || !existing->isValid || existing->isUsed)
    throw ResponseException(AuthErrors::RefreshTokenInvalidOrExpired);

  if (existing->expiresAt <= static_cast<int64_t>(std::time(nullptr))) {
    LOG_WARN << "Auth: expired refresh token for user " << *userId;
    throw ResponseException(AuthErrors::RefreshTokenInvalidOrExpired);
  }

  const bool sameDevice = !DeviceFilter::credentialMode() ||
                          existing->deviceHash == input.deviceHash;
  if (existing->userAgent != input.userAgent || !sameDevice) {
    LOG_WARN << "Auth: device mismatch on refresh for user " << *userId;
    throw ResponseException(AuthErrors::RefreshTokenInvalidOrExpired);
  }

  std::map<std::string, std::string> newClaims;
  newClaims["sub"] = std::to_string(*userId);

  ResponseRefreshTokenDto result;
  result.accessToken = dependencies_.jwtService.generateAccess(newClaims);
  result.refreshToken = dependencies_.jwtService.generateRefresh(newClaims);

  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    if (!co_await dependencies_.refreshTokenRepository.markUsed(
            existing->id, transaction.get()))
      throw ResponseException(AuthErrors::RefreshTokenInvalidOrExpired);
    co_await dependencies_.refreshTokenRepository.pruneStale(*userId,
                                                             transaction.get());
    co_await dependencies_.refreshTokenRepository.create(
        {.userId = *userId,
         .accessToken = result.accessToken,
         .refreshToken = result.refreshToken,
         .deviceHash = input.deviceHash,
         .userAgent = existing->userAgent,
         .expiresAt = static_cast<int64_t>(std::time(nullptr)) +
                      dependencies_.jwtService.refreshTtlSeconds(),
         .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(AuthErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return result;
}

drogon::Task<void> AuthFeatureService::logout(const LogoutInput& input) const
{
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    co_await dependencies_.refreshTokenRepository.invalidateAllUser(
        input.userId, transaction.get());

    if (const auto* sink = auth_change::getSink()) {
      co_await sink->publishAction(
          {.event = {.userId = input.userId,
                     .recordId = input.userId,
                     .tableName = TableName::User,
                     .action = UserAction::Delete,
                     .oldData = Json::Value(),
                     .newData = Json::Value(),
                     .ipAddress = ""},
           .client = transaction.get()});
    }

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(AuthErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  SocketEmitDto context;
  context.operation = SyncOperation::AuthContextChanged;
  context.option = TableName::User;
  context.obj = Json::Value(Json::objectValue);
  context.obj["id"] = static_cast<Json::Int64>(input.userId);
  context.obj["name"] = input.name;
  context.obj["role"] = userRoleToString(input.role);
  context.obj["isActive"] = false;
  context.obj["resync"] = false;
  if (const auto* control = sync_control::getSink()) {
    if (!control->disconnectUser(input.userId, context))
      LOG_WARN << "Auth: disconnect failed for user " << input.userId;
  }

  LOG_INFO << "Auth: logged out user " << input.userId;
}

drogon::Task<void>
AuthFeatureService::updateMe(const UpdateMeInput& input) const
{
  if (!input.name || input.name->empty())
    co_return;

  const auto answer = co_await renameIdentityUser(
      dependencies_.identity,
      {.userId = input.userId, .name = *input.name, .role = input.role});
  if (!answer)
    throw ResponseException(AuthErrors::UserNotFound);
}

drogon::Task<ResponseLoginDto>
AuthFeatureService::issueSession(const IssueSessionInput& input) const
{
  ResponseLoginDto result;
  IssuedDeviceCredential credential;
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    credential = co_await issueDeviceCredential(
        {.userId = input.user.userId,
         .userAgent = input.device.userAgent,
         .client = transaction.get()});
    const std::string deviceHash = credential.deviceHash.empty()
                                       ? input.device.deviceHash
                                       : credential.deviceHash;

    std::map<std::string, std::string> claims;
    claims["sub"] = std::to_string(input.user.userId);

    result.accessToken = dependencies_.jwtService.generateAccess(claims);
    result.refreshToken = dependencies_.jwtService.generateRefresh(claims);
    result.userId = input.user.userId;
    result.name = input.user.name + " " + input.user.lastName;
    result.role = input.user.role;
    result.personId = input.personId;
    result.deviceSecret = credential.secret;

    co_await dependencies_.refreshTokenRepository.create(
        {.userId = input.user.userId,
         .accessToken = result.accessToken,
         .refreshToken = result.refreshToken,
         .deviceHash = deviceHash,
         .userAgent = input.device.userAgent,
         .expiresAt = static_cast<int64_t>(std::time(nullptr)) +
                      dependencies_.jwtService.refreshTtlSeconds(),
         .client = transaction.get()});

    Json::Value session(Json::objectValue);
    session["deviceHash"] = deviceHash;
    session["userAgent"] = input.device.userAgent;

    if (const auto* sink = auth_change::getSink()) {
      co_await sink->publishAction(
          {.event = {.userId = input.user.userId,
                     .recordId = input.user.userId,
                     .tableName = TableName::User,
                     .action = UserAction::Create,
                     .oldData = Json::Value(),
                     .newData = session,
                     .ipAddress = ""},
           .client = transaction.get()});
    }

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(AuthErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  co_return result;
}

drogon::Task<IssuedDeviceCredential>
AuthFeatureService::issueDeviceCredential(
    const IssueDeviceCredentialInput& input) const
{
  IssuedDeviceCredential issued;
  if (!DeviceFilter::credentialMode())
    co_return issued;

  issued.secret = randomSecret();
  if (issued.secret.empty())
    throw ResponseException(AuthErrors::DeviceCredentialIssuanceFailed);

  const std::string secretHash = DeviceFilter::sha256Hex(issued.secret);
  issued.deviceHash =
      DeviceFilter::credentialFingerprint(input.userAgent, secretHash);
  co_await dependencies_.deviceCredentialRepository.create(
      {.userId = input.userId,
       .deviceHash = issued.deviceHash,
       .secretHash = secretHash,
       .client = input.client});
  co_return issued;
}
