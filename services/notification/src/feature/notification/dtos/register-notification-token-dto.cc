#include "register-notification-token-dto.hxx"

#include <algorithm>
#include <cctype>
#include <push/push-provider.hxx>

#include <cstddef>
#include <optional>

namespace
{
constexpr std::size_t kPublicKeyLength = 43;
constexpr std::size_t kKeyIdLength = 22;

bool isBase64UrlOfLength(const std::string& text, std::size_t length)
{
  return text.size() == length &&
         std::ranges::all_of(text, [](unsigned char c) {
           return std::isalnum(c) != 0 || c == '-' || c == '_';
         });
}

bool anyPushField(const RegisterNotificationTokenDto& value)
{
  return !value.provider.empty() || !value.publicKey.empty() || !value.keyId.empty();
}
}

RegisterNotificationTokenDto
RegisterNotificationTokenDto::fromJson(const Json::Value& json)
{
  RegisterNotificationTokenDto dto;
  dto.token = json.get("token", "").asString();
  dto.platform = json.get("platform", "").asString();
  dto.lang = json.get("lang", "").asString();
  dto.provider = json.get("provider", "").asString();
  dto.publicKey = json.get("publicKey", "").asString();
  dto.keyId = json.get("keyId", "").asString();

  START_VALIDATION(RegisterNotificationTokenDto, dto)
  IS_NOT_EMPTY(token)
  MAX_LENGTH(token, 255)
  MAX_LENGTH(platform, 32)
  MAX_LENGTH(lang, 8)
  CUSTOM_LAMBDA(provider, [](const RegisterNotificationTokenDto& value) -> std::optional<std::string> {
    if (value.provider.empty() || pushProviderFromString(value.provider))
      return std::nullopt;
    return "provider must be fcm, apns or apns_voip";
  })
  CUSTOM_LAMBDA(publicKey, [](const RegisterNotificationTokenDto& value) -> std::optional<std::string> {
    if (value.publicKey.empty() || isBase64UrlOfLength(value.publicKey, kPublicKeyLength))
      return std::nullopt;
    return "publicKey must be 32 bytes in base64url";
  })
  CUSTOM_LAMBDA(keyId, [](const RegisterNotificationTokenDto& value) -> std::optional<std::string> {
    if (value.keyId.empty() || isBase64UrlOfLength(value.keyId, kKeyIdLength))
      return std::nullopt;
    return "keyId must be 16 bytes in base64url";
  })
  CUSTOM_LAMBDA(pushFields, [](const RegisterNotificationTokenDto& value) -> std::optional<std::string> {
    if (!anyPushField(value) || (!value.provider.empty() && !value.publicKey.empty() && !value.keyId.empty()))
      return std::nullopt;
    return "provider, publicKey and keyId travel together";
  })
  END_VALIDATION()

  return dto;
}
