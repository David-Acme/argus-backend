#pragma once

#include <push/push-envelope.hxx>

#include <cstdint>
#include <string>

enum class PushSealStatus : uint8_t
{
  Sealed = 0,
  KeyInvalid,
  IntentTooLarge,
  TokenTooLong,
  SealFailed
};

std::string_view pushSealStatusToString(PushSealStatus status) noexcept;

struct PushSealRequest
{
  std::string deliveryId;
  PushProvider provider{PushProvider::Fcm};
  PushKind kind{PushKind::Call};
  PushPriority priority{PushPriority::High};
  uint32_t ttlSeconds{0};
  std::string collapseId;
  std::string token;
  std::string publicKey;
  std::string keyId;
  std::string plaintext;
};

struct PushSealOutcome
{
  PushSealStatus status{PushSealStatus::SealFailed};
  PushSealedEnvelope envelope;
};

std::string randomBytes(std::size_t count);

class PushSealer
{
public:
  [[nodiscard]] PushSealOutcome seal(const PushSealRequest& request) const;
};
