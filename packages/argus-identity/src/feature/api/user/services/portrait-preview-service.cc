#include "portrait-preview-service.hxx"

#include <config/app-config.hxx>
#include <array>
#include <ctime>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <shared/exceptions/response-exception.hxx>
#include <string_view>

namespace
{
constexpr int64_t kCapabilityLifetimeSeconds = 60;

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
    throw ResponseException("Unable to prepare portrait preview", 503,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);
  output.resize(static_cast<size_t>(written));
  return output;
}
} // namespace

void PortraitPreviewService::requireAccess(UserRole role)
{
  if (role != UserRole::Owner && role != UserRole::Guard)
    throw ResponseException("Portrait verification is not available", 403,
                            AppConfig::ERROR_CODE_FORBIDDEN);
}

std::string PortraitPreviewService::hashToken(const std::string& token)
{
  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int length = 0;
  if (EVP_Digest(token.data(), token.size(), digest, &length, EVP_sha256(),
                 nullptr) != 1)
    throw ResponseException("Unable to prepare portrait preview", 503,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);
  return hexDigest(digest, length);
}

std::string PortraitPreviewService::newToken()
{
  std::array<unsigned char, 32> bytes{};
  if (RAND_bytes(bytes.data(), bytes.size()) != 1)
    throw ResponseException("Unable to prepare portrait preview", 503,
                            AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE);

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

drogon::Task<ResponsePortraitPreviewCapabilityDto>
PortraitPreviewService::create(const PortraitPreviewCreateInput& input) const
{
  requireAccess(input.requesterRole);
  if (input.portraitUserId <= 0)
    throw ResponseException("Invalid user id", 400, AppConfig::ERROR_CODE_BAD_REQUEST);

  const auto target = co_await userRepository_.findById(input.portraitUserId);
  if (!target)
    throw ResponseException("User not found", 404, AppConfig::ERROR_CODE_NOT_FOUND);
  if (!co_await privatePortraitService_.has(target->id))
    throw ResponseException("Portrait is unavailable", 404,
                            AppConfig::ERROR_CODE_NOT_FOUND);

  const auto token = newToken();
  const auto expiresAt = std::time(nullptr) + kCapabilityLifetimeSeconds;
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
    throw ResponseException("Portrait preview is unavailable", 404,
                            AppConfig::ERROR_CODE_NOT_FOUND);

  const auto capability =
      co_await capabilityRepository_.findByTokenHash(hashToken(input.token));
  const auto now = std::time(nullptr);
  if (!capability || capability->requesterUserId != input.requesterUserId) {
    throw ResponseException("Portrait preview is unavailable", 404,
                            AppConfig::ERROR_CODE_NOT_FOUND);
  }
  const bool consumed = co_await capabilityRepository_.tryConsume({
      .id = capability->id,
      .requesterUserId = input.requesterUserId,
      .now = now,
  });
  if (!consumed)
    throw ResponseException("Portrait preview is unavailable", 404,
                            AppConfig::ERROR_CODE_NOT_FOUND);

  const auto portrait =
      co_await privatePortraitService_.read(capability->portraitUserId);
  if (!portrait)
    throw ResponseException("Portrait is unavailable", 404,
                            AppConfig::ERROR_CODE_NOT_FOUND);

  Json::Value event;
  event["event"] = "portrait_preview";
  event["portraitUserId"] = Json::Int64(capability->portraitUserId);
  co_await userActionLogService_.record({
      .userId = input.requesterUserId,
      .recordId = capability->portraitUserId,
      .tableName = TableName::User,
      .action = UserAction::Read,
      .oldData = Json::Value(),
      .newData = event,
      .ipAddress = "",
  });
  co_return {
      .mimeType = portrait->mimeType,
      .base64 = base64(portrait->bytes),
  };
}
