#include "invitation-feature-service.hxx"

#include <config/app-config.hxx>
#include <ctime>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <shared/exceptions/response-exception.hxx>
#include <shared/services/cert/cert-service.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>

namespace
{
std::string hexDigest(const unsigned char* bytes, unsigned int length)
{
  static constexpr char kHex[] = "0123456789abcdef";
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
  unsigned char bytes[32]{};
  if (RAND_bytes(bytes, sizeof(bytes)) != 1)
    throw ResponseException("Unable to create invitation", 503,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);

  static constexpr char kAlphabet[] =
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
} // namespace

std::string InvitationFeatureService::hashToken(const std::string& token)
{
  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int length = 0;
  if (EVP_Digest(token.data(), token.size(), digest, &length, EVP_sha256(),
                 nullptr) != 1)
    throw ResponseException("Unable to resolve invitation", 503,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);
  return hexDigest(digest, length);
}

drogon::Task<ResponseInvitationDto>
InvitationFeatureService::create(const CreateInvitationDto& body,
                                 int64_t actorId) const
{
  if (body.role == UserRole::Owner)
    throw ResponseException("Invitations cannot grant owner access", 422,
                            AppConfig::ERROR_CODE_BAD_REQUEST);

  const auto token = newOpaqueToken();
  const auto invitation = co_await repository_.create({
      .tokenHash = hashToken(token),
      .role = body.role,
      .maxRedemptions = body.maxRedemptions,
      .expiresAt = body.expiresAt,
      .createdBy = actorId,
  });
  emitInvitation(invitation);
  co_await recordInvitationAction({
      .actorId = actorId,
      .before = {},
      .after = invitation,
      .action = UserAction::Create,
  });
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
  const auto before = co_await repository_.findById(invitationId);
  if (!before)
    throw ResponseException("Invitation not found", 404,
                            AppConfig::ERROR_CODE_NOT_FOUND);
  const bool revoked = co_await repository_.revoke({
      .invitationId = invitationId,
      .revokedBy = actorId,
  });
  if (!revoked)
    throw ResponseException("Invitation not found", 404,
                            AppConfig::ERROR_CODE_NOT_FOUND);
  const auto after = co_await repository_.findById(invitationId);
  if (!after)
    throw ResponseException("Invitation not found", 404,
                            AppConfig::ERROR_CODE_NOT_FOUND);
  emitInvitation(*after);
  co_await recordInvitationAction({
      .actorId = actorId,
      .before = *before,
      .after = *after,
      .action = UserAction::Update,
  });
  co_return;
}

void InvitationFeatureService::emitInvitation(
    const UserInvitationSchema& invitation) const
{
  SocketEmitDto body;
  body.operation = SyncOperation::Add;
  body.option = TableName::UserInvitation;
  body.obj = invitation.toJson();
  socketService_.emitModule(TableName::UserInvitation, body);
}

drogon::Task<void> InvitationFeatureService::recordInvitationAction(
    const InvitationActionLogInput& input) const
{
  co_await userActionLogService_.record({
      .userId = input.actorId,
      .recordId = input.after.id,
      .tableName = TableName::UserInvitation,
      .action = input.action,
      .oldData = input.before.id == 0 ? Json::Value() : input.before.toJson(),
      .newData = input.after.toJson(),
      .ipAddress = "",
  });
  co_return;
}

drogon::Task<ResponseInvitationResolveDto>
InvitationFeatureService::resolve(const std::string& token) const
{
  if (!ConfigService::getBool("pairing.paired"))
    throw ResponseException("Server is not paired yet", 409,
                            AppConfig::ERROR_CODE_CONFLICT);
  if (!CertService::isLoaded())
    throw ResponseException("Server certificate is unavailable", 503,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);

  const auto invitation = co_await repository_.findByTokenHash(hashToken(token));
  if (!invitation || !isUsable(*invitation, std::time(nullptr)))
    throw ResponseException("Invitation is invalid or expired", 404,
                            AppConfig::ERROR_CODE_NOT_FOUND);

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
