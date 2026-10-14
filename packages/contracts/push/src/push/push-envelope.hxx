#pragma once

#include <push/push-kind.hxx>
#include <push/push-outcome.hxx>
#include <push/push-priority.hxx>
#include <push/push-provider.hxx>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace push_envelope
{
inline constexpr std::size_t kDeliveryIdSize = 16;
inline constexpr std::size_t kEncSize = 32;
inline constexpr std::size_t kMinCiphertextSize = 16;
inline constexpr std::size_t kMaxCiphertextSize = 2048;
inline constexpr std::size_t kMaxCollapseIdSize = 64;
inline constexpr std::size_t kMaxTokenSize = 512;
inline constexpr std::size_t kResultPayloadSize = 17;
inline constexpr std::string_view kInfoPrefix = "argus.push.v1";

inline void appendU16(std::string& out, std::size_t value)
{
  out.push_back(static_cast<char>(value & 0xFFU));
  out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

inline void appendU32(std::string& out, uint32_t value)
{
  for (uint32_t shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
}

class ByteCursor
{
public:
  explicit ByteCursor(std::string_view bytes) : bytes_(bytes) {}

  bool take(std::size_t count, std::string& out)
  {
    if (bytes_.size() - offset_ < count)
      return false;
    out.assign(bytes_.substr(offset_, count));
    offset_ += count;
    return true;
  }

  bool byte(uint8_t& out)
  {
    if (offset_ >= bytes_.size())
      return false;
    out = static_cast<uint8_t>(bytes_[offset_]);
    ++offset_;
    return true;
  }

  bool u16(std::size_t& out)
  {
    uint8_t low = 0;
    uint8_t high = 0;
    if (!byte(low) || !byte(high))
      return false;
    out = static_cast<std::size_t>(low) | (static_cast<std::size_t>(high) << 8U);
    return true;
  }

  bool u32(uint32_t& out)
  {
    out = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
      uint8_t next = 0;
      if (!byte(next))
        return false;
      out |= static_cast<uint32_t>(next) << shift;
    }
    return true;
  }

  [[nodiscard]] bool done() const { return offset_ == bytes_.size(); }

private:
  std::string_view bytes_;
  std::size_t offset_{0};
};

inline bool combinationAllowed(PushProvider provider, PushKind kind) noexcept
{
  if (provider == PushProvider::Apns)
    return kind == PushKind::Alert;
  if (provider == PushProvider::ApnsVoip)
    return kind == PushKind::Call;
  return true;
}
}

struct PushSealedEnvelope
{
  std::string deliveryId;
  PushProvider provider{PushProvider::Fcm};
  PushKind kind{PushKind::Alert};
  PushPriority priority{PushPriority::Normal};
  uint32_t ttlSeconds{0};
  std::string collapseId;
  std::string token;
  std::string enc;
  std::string ciphertext;
};

struct PushResultEnvelope
{
  std::string deliveryId;
  PushOutcome outcome{PushOutcome::Failed};
};

namespace push_envelope
{
inline bool wellFormed(const PushSealedEnvelope& envelope) noexcept
{
  return envelope.deliveryId.size() == kDeliveryIdSize &&
         envelope.collapseId.size() <= kMaxCollapseIdSize &&
         !envelope.token.empty() && envelope.token.size() <= kMaxTokenSize &&
         envelope.enc.size() == kEncSize &&
         envelope.ciphertext.size() >= kMinCiphertextSize &&
         envelope.ciphertext.size() <= kMaxCiphertextSize &&
         combinationAllowed(envelope.provider, envelope.kind);
}

inline std::string envelopePrefix(const PushSealedEnvelope& envelope)
{
  std::string out;
  out.reserve(kDeliveryIdSize + 16 + envelope.collapseId.size() + envelope.token.size());
  out.append(envelope.deliveryId);
  out.push_back(static_cast<char>(envelope.provider));
  out.push_back(static_cast<char>(envelope.kind));
  out.push_back(static_cast<char>(envelope.priority));
  appendU32(out, envelope.ttlSeconds);
  out.push_back(static_cast<char>(envelope.collapseId.size()));
  out.append(envelope.collapseId);
  appendU16(out, envelope.token.size());
  out.append(envelope.token);
  return out;
}

inline std::string hpkeInfo(std::string_view keyId, const PushSealedEnvelope& envelope)
{
  std::string out(kInfoPrefix);
  out.push_back('\0');
  out.append(keyId);
  out.push_back('\0');
  out.append(envelopePrefix(envelope));
  return out;
}

inline std::optional<std::string>
sealedPayload(const PushSealedEnvelope& envelope)
{
  if (!wellFormed(envelope))
    return std::nullopt;
  std::string out = envelopePrefix(envelope);
  appendU16(out, envelope.enc.size());
  out.append(envelope.enc);
  appendU16(out, envelope.ciphertext.size());
  out.append(envelope.ciphertext);
  return out;
}

inline bool parseSealedPayload(std::string_view payload, PushSealedEnvelope& out)
{
  ByteCursor cursor{payload};
  std::string deliveryId;
  uint8_t provider = 0;
  uint8_t kind = 0;
  uint8_t priority = 0;
  uint32_t ttl = 0;
  uint8_t collapseLength = 0;
  std::size_t tokenLength = 0;
  std::size_t encLength = 0;
  std::size_t ciphertextLength = 0;
  PushSealedEnvelope parsed;
  if (!cursor.take(kDeliveryIdSize, deliveryId) || !cursor.byte(provider) ||
      !cursor.byte(kind) || !cursor.byte(priority) || !cursor.u32(ttl) ||
      !cursor.byte(collapseLength))
    return false;
  if (provider < 1 || provider > 3 || kind < 1 || kind > 2 || priority > 1 ||
      collapseLength > kMaxCollapseIdSize)
    return false;
  if (!cursor.take(collapseLength, parsed.collapseId) || !cursor.u16(tokenLength))
    return false;
  if (tokenLength == 0 || tokenLength > kMaxTokenSize ||
      !cursor.take(tokenLength, parsed.token) || !cursor.u16(encLength))
    return false;
  if (encLength != kEncSize || !cursor.take(encLength, parsed.enc) ||
      !cursor.u16(ciphertextLength))
    return false;
  if (ciphertextLength < kMinCiphertextSize || ciphertextLength > kMaxCiphertextSize ||
      !cursor.take(ciphertextLength, parsed.ciphertext) || !cursor.done())
    return false;
  parsed.deliveryId = std::move(deliveryId);
  parsed.provider = static_cast<PushProvider>(provider);
  parsed.kind = static_cast<PushKind>(kind);
  parsed.priority = static_cast<PushPriority>(priority);
  parsed.ttlSeconds = ttl;
  if (!wellFormed(parsed))
    return false;
  out = std::move(parsed);
  return true;
}

inline std::optional<std::string>
resultPayload(const PushResultEnvelope& envelope)
{
  if (envelope.deliveryId.size() != kDeliveryIdSize)
    return std::nullopt;
  std::string out = envelope.deliveryId;
  out.push_back(static_cast<char>(envelope.outcome));
  return out;
}

inline bool parseResultPayload(std::string_view payload, PushResultEnvelope& out)
{
  if (payload.size() != kResultPayloadSize)
    return false;
  const auto raw = static_cast<uint8_t>(payload[kDeliveryIdSize]);
  if (raw > static_cast<uint8_t>(PushOutcome::Failed))
    return false;
  out.deliveryId.assign(payload.substr(0, kDeliveryIdSize));
  out.outcome = static_cast<PushOutcome>(raw);
  return true;
}
}
