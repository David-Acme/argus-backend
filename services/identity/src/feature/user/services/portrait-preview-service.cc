#include "portrait-preview-service.hxx"

#include <auth/module-gate.hxx>
#include <auth/role-access.hxx>
#include <ctime>
#include <openssl/evp.h>
#include <identity/identity-errors.hxx>
#include <errors/response-exception.hxx>
#include <optional>
#include <shared/services/token/opaque-token.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <string_view>
#include <sync/identity-change-sink.hxx>

namespace
{
constexpr int64_t kCapabilityLifetimeSeconds = 60;

std::string base64(const std::string& input)
{
  if (input.empty())
    return {};
  std::string output(((input.size() + 2) / 3) * 4, '\0');
  const auto written = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(output.data()),
      reinterpret_cast<const unsigned char*>(input.data()),
      static_cast<int>(input.size()));
  if (written <= 0)
    throw ResponseException(503, IdentityErrors::PortraitPreviewPreparationFailed);
  output.resize(static_cast<size_t>(written));
  return output;
}
}

void PortraitPreviewService::requireAccess(UserRole role)
{
  if (!role_access::readsUserDirectory(role, moduleGate().roleActive(role)))
    throw ResponseException(403, IdentityErrors::PortraitVerificationUnavailable);
}

std::string PortraitPreviewService::hashToken(const std::string& token)
{
  auto hash = opaque_token::sha256Hex(token);
  if (!hash)
    throw ResponseException(503, IdentityErrors::PortraitPreviewPreparationFailed);
  return std::move(*hash);
}

std::string PortraitPreviewService::newToken()
{
  auto token = opaque_token::mint();
  if (!token)
    throw ResponseException(503, IdentityErrors::PortraitPreviewPreparationFailed);
  return std::move(*token);
}

drogon::Task<ResponsePortraitPreviewCapabilityDto>
PortraitPreviewService::create(const PortraitPreviewCreateInput& input) const
{
  requireAccess(input.requesterRole);
  if (input.portraitUserId <= 0)
    throw ResponseException(400, IdentityErrors::InvalidUserId);

  const auto target = co_await userRepository_.findById(input.portraitUserId);
  if (!target)
    throw ResponseException(404, IdentityErrors::UserNotFound);
  if (!co_await privatePortraitService_.has(target->id))
    throw ResponseException(404, IdentityErrors::PortraitUnavailable);

  const auto token = newToken();
  const int64_t now = std::time(nullptr);
  const auto expiresAt = now + kCapabilityLifetimeSeconds;
  co_await capabilityRepository_.purgeSpent(now);
  co_await capabilityRepository_.create({
      .tokenHash = hashToken(token),
      .portraitUserId = target->id,
      .requesterUserId = input.requesterUserId,
      .expiresAt = expiresAt,
  });
  co_return {.token = token, .expiresAt = expiresAt};
}

drogon::Task<ResponsePortraitPreviewImageDto>
PortraitPreviewService::consume(const PortraitPreviewConsumeInput& input) const
{
  requireAccess(input.requesterRole);
  if (input.token.empty())
    throw ResponseException(404, IdentityErrors::PortraitPreviewUnavailable);

  std::optional<PortraitPreviewCapabilitySchema> capability;
  auto transaction =
      co_await db_transaction::begin(DbService::identityClient());
  try {
    capability = co_await capabilityRepository_.findByTokenHash(
        hashToken(input.token), transaction.get());
    const auto now = std::time(nullptr);
    if (!capability || capability->requesterUserId != input.requesterUserId)
      throw ResponseException(404,
                              IdentityErrors::PortraitPreviewUnavailable);

    const bool consumed = co_await capabilityRepository_.tryConsume({
        .id = capability->id,
        .requesterUserId = input.requesterUserId,
        .now = now,
        .client = transaction.get(),
    });
    if (!consumed)
      throw ResponseException(404,
                              IdentityErrors::PortraitPreviewUnavailable);

    Json::Value event;
    event["event"] = "portrait_preview";
    event["portraitUserId"] = Json::Int64(capability->portraitUserId);
    if (const auto* sink = identity_change::getSink()) {
      co_await sink->publishAction(
          {.event = {.userId = input.requesterUserId,
                     .recordId = capability->portraitUserId,
                     .tableName = TableName::User,
                     .action = UserAction::Read,
                     .oldData = Json::Value(),
                     .newData = event,
                     .ipAddress = ""},
           .client = transaction.get()});
    }

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(IdentityErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  const auto portrait =
      co_await privatePortraitService_.read(capability->portraitUserId);
  if (!portrait)
    throw ResponseException(404, IdentityErrors::PortraitUnavailable);

  co_return {
      .mimeType = portrait->mimeType,
      .base64 = base64(portrait->bytes),
  };
}
