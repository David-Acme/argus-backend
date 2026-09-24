#include "invitation-feature-service.hxx"

#include <array>
#include <ctime>
#include <errors/response-exception.hxx>
#include <identity/identity-errors.hxx>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <optional>
#include <string_view>
#include <cert/cert-service.hxx>
#include <config/config-service.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>
#include <sync/sync-operation.hxx>
#include <sync/socket-emit-dto.hxx>

namespace
{
std::string hexDigest(const unsigned char* bytes, unsigned int length)
{
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string output;
  output.reserve(static_cast<size_t>(length) * 2);
  for (unsigned int i = 0; i < length; ++i) {
    output.push_back(kHex[bytes[i] >> 4U]);
    output.push_back(kHex[bytes[i] & 0x0FU]);
  }
  return output;
}

std::string newOpaqueToken()
{
  std::array<unsigned char, 32> bytes{};
  if (RAND_bytes(bytes.data(), bytes.size()) != 1)
    throw ResponseException(503, IdentityErrors::InvitationCreationFailed);

  static constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string token;
  token.reserve(43);
  uint32_t accumulator = 0;
  int bits = 0;
  for (const auto byte : bytes) {
    accumulator = (accumulator << 8U) | byte;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      token.push_back(kAlphabet[(accumulator >> bits) & 0x3FU]);
    }
  }
  if (bits > 0)
    token.push_back(kAlphabet[(accumulator << (6 - bits)) & 0x3FU]);
  return token;
}

bool isUsable(const UserInvitationSchema& invitation, int64_t now)
{
  return !invitation.revokedAt && invitation.expiresAt > now &&
         invitation.redemptionCount < invitation.maxRedemptions;
}
}

std::string InvitationFeatureService::hashToken(const std::string& token)
{
  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int length = 0;
  if (EVP_Digest(token.data(), token.size(), digest, &length, EVP_sha256(),
                 nullptr) != 1)
    throw ResponseException(503, IdentityErrors::InvitationResolutionFailed);
  return hexDigest(digest, length);
}

drogon::Task<ResponseInvitationDto>
InvitationFeatureService::create(const CreateInvitationDto& body,
                                 int64_t actorId) const
{
  if (body.role == UserRole::Owner)
    throw ResponseException(422, IdentityErrors::InvitationOwnerAccessForbidden);

  const auto token = newOpaqueToken();

  UserInvitationSchema invitation;
  auto transaction =
      co_await db_transaction::begin(DbService::identityClient());
  try {
    invitation = co_await repository_.create({
        .tokenHash = hashToken(token),
        .role = body.role,
        .maxRedemptions = body.maxRedemptions,
        .expiresAt = body.expiresAt,
        .createdBy = actorId,
        .client = transaction.get(),
    });
    co_await emitInvitation(
        {.invitation = invitation, .client = transaction.get()});
    co_await recordInvitationAction({
        .actorId = actorId,
        .before = {},
        .after = invitation,
        .action = UserAction::Create,
        .client = transaction.get(),
    });
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return ResponseInvitationDto{.invitation = invitation, .token = token};
}

drogon::Task<std::vector<ResponseInvitationDto>>
InvitationFeatureService::list() const
{
  const auto invitations = co_await repository_.findAll();
  std::vector<ResponseInvitationDto> result;
  result.reserve(invitations.size());
  for (const auto& invitation : invitations)
    result.push_back({.invitation = invitation, .token = ""});
  co_return result;
}

drogon::Task<void>
InvitationFeatureService::revoke(int64_t invitationId, int64_t actorId) const
{
  std::optional<UserInvitationSchema> before;
  UserInvitationSchema after;

  auto transaction =
      co_await db_transaction::begin(DbService::identityClient());
  try {
    before = co_await repository_.findById(invitationId, transaction.get());
    if (!before)
      throw ResponseException(404, IdentityErrors::InvitationNotFound);
    const bool revoked = co_await repository_.revoke({
        .invitationId = invitationId,
        .revokedBy = actorId,
        .client = transaction.get(),
    });
    if (!revoked)
      throw ResponseException(404, IdentityErrors::InvitationNotFound);
    const auto updated = co_await repository_.findById(invitationId,
                                                       transaction.get());
    if (!updated)
      throw ResponseException(404, IdentityErrors::InvitationNotFound);
    after = *updated;
    if (const auto* sink = identity_change::getSink()) {
      co_await sink->publishModuleAudit({
          .recordId = after.id,
          .tableName = TableName::UserInvitation,
          .before = before->toJson(),
          .after = after.toJson(),
          .actorId = actorId,
          .client = transaction.get(),
      });
    }
    co_await recordInvitationAction({
        .actorId = actorId,
        .before = *before,
        .after = after,
        .action = UserAction::Update,
        .client = transaction.get(),
    });
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return;
}

drogon::Task<void> InvitationFeatureService::emitInvitation(
    const InvitationEmitInput& input) const
{
  SocketEmitDto body;
  body.operation = SyncOperation::Add;
  body.option = TableName::UserInvitation;
  body.obj = input.invitation.toJson();
  if (const auto* sink = identity_change::getSink())
    co_await sink->emitModule({.table = TableName::UserInvitation,
                               .body = body,
                               .client = input.client});
  co_return;
}

drogon::Task<void> InvitationFeatureService::recordInvitationAction(
    const InvitationActionLogInput& input) const
{
  if (const auto* sink = identity_change::getSink()) {
    co_await sink->publishAction(
        {.event =
             {.userId = input.actorId,
              .recordId = input.after.id,
              .tableName = TableName::UserInvitation,
              .action = input.action,
              .oldData =
                  input.before.id == 0 ? Json::Value() : input.before.toJson(),
              .newData = input.after.toJson(),
              .ipAddress = ""},
         .client = input.client});
  }
  co_return;
}

drogon::Task<ResponseInvitationResolveDto>
InvitationFeatureService::resolve(const std::string& token) const
{
  if (!ConfigService::getBool("pairing.paired"))
    throw ResponseException(409, IdentityErrors::ServerNotPaired);
  if (!CertService::isLoaded())
    throw ResponseException(503, IdentityErrors::ServerCertificateUnavailable);

  const auto invitation = co_await repository_.findByTokenHash(hashToken(token));
  if (!invitation || !isUsable(*invitation, std::time(nullptr)))
    throw ResponseException(404, IdentityErrors::InvitationInvalidOrExpired);

  ResponseInvitationResolveDto result;
  result.role = invitation->role;
  result.expiresAt = invitation->expiresAt;
  result.instanceId = CertService::instanceId();
  result.caFingerprint = CertService::caFingerprint();
  result.serverFingerprint = CertService::serverFingerprint();
  result.caPem = CertService::caPem();
  result.scheme = "https";
  result.port = ConfigService::getInt("mdns.port");
  co_return result;
}
