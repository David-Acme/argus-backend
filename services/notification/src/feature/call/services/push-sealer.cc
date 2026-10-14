#include <feature/call/services/push-sealer.hxx>

#include <text/base64.hxx>

#include <openssl/evp.h>
#include <openssl/hpke.h>
#include <openssl/rand.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>

namespace
{
constexpr std::size_t kRawPublicKeySize = 32;
constexpr std::size_t kMaxRegisteredTokenSize = 255;

const OSSL_HPKE_SUITE kSuite{.kem_id = OSSL_HPKE_KEM_ID_X25519,
                             .kdf_id = OSSL_HPKE_KDF_ID_HKDF_SHA256,
                             .aead_id = OSSL_HPKE_AEAD_ID_CHACHA_POLY1305};

struct HpkeContextDeleter
{
  void operator()(OSSL_HPKE_CTX* context) const noexcept
  {
    OSSL_HPKE_CTX_free(context);
  }
};

using HpkeContextPtr = std::unique_ptr<OSSL_HPKE_CTX, HpkeContextDeleter>;

std::optional<std::string> decodePublicKey(const std::string& base64url)
{
  std::string standard = base64url;
  std::ranges::replace(standard, '-', '+');
  std::ranges::replace(standard, '_', '/');
  while (standard.size() % 4 != 0)
    standard.push_back('=');
  auto raw = base64::decode(standard);
  if (!raw || raw->size() != kRawPublicKeySize)
    return std::nullopt;
  return raw;
}

const unsigned char* bytesOf(const std::string& text) noexcept
{
  return reinterpret_cast<const unsigned char*>(text.data());
}
}

std::string_view pushSealStatusToString(PushSealStatus status) noexcept
{
  switch (status) {
    case PushSealStatus::Sealed:
      return "sealed";
    case PushSealStatus::KeyInvalid:
      return "key_invalid";
    case PushSealStatus::IntentTooLarge:
      return "intent_too_large";
    case PushSealStatus::TokenTooLong:
      return "token_too_long";
    case PushSealStatus::SealFailed:
      return "seal_failed";
  }
  return "";
}

std::string randomBytes(std::size_t count)
{
  std::string out(count, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(count)) != 1)
    out.clear();
  return out;
}

PushSealOutcome PushSealer::seal(const PushSealRequest& request) const
{
  PushSealOutcome outcome;
  if (request.token.size() > kMaxRegisteredTokenSize) {
    outcome.status = PushSealStatus::TokenTooLong;
    return outcome;
  }
  const auto publicKey = decodePublicKey(request.publicKey);
  if (!publicKey) {
    outcome.status = PushSealStatus::KeyInvalid;
    return outcome;
  }
  const std::size_t ciphertextSize =
      OSSL_HPKE_get_ciphertext_size(kSuite, request.plaintext.size());
  if (ciphertextSize > push_envelope::kMaxCiphertextSize) {
    outcome.status = PushSealStatus::IntentTooLarge;
    return outcome;
  }

  PushSealedEnvelope envelope{.deliveryId = request.deliveryId,
                              .provider = request.provider,
                              .kind = request.kind,
                              .priority = request.priority,
                              .ttlSeconds = request.ttlSeconds,
                              .collapseId = request.collapseId,
                              .token = request.token,
                              .enc = {},
                              .ciphertext = {}};
  const std::string info = push_envelope::hpkeInfo(request.keyId, envelope);

  HpkeContextPtr context{OSSL_HPKE_CTX_new(OSSL_HPKE_MODE_BASE, kSuite, OSSL_HPKE_ROLE_SENDER,
                                           nullptr, nullptr)};
  if (!context) {
    outcome.status = PushSealStatus::SealFailed;
    return outcome;
  }

  std::string enc(OSSL_HPKE_get_public_encap_size(kSuite), '\0');
  size_t encLength = enc.size();
  if (OSSL_HPKE_encap(context.get(), reinterpret_cast<unsigned char*>(enc.data()), &encLength,
                      bytesOf(*publicKey), publicKey->size(), bytesOf(info), info.size()) != 1) {
    outcome.status = PushSealStatus::SealFailed;
    return outcome;
  }
  enc.resize(encLength);

  std::string ciphertext(ciphertextSize, '\0');
  size_t ciphertextLength = ciphertext.size();
  if (OSSL_HPKE_seal(context.get(), reinterpret_cast<unsigned char*>(ciphertext.data()),
                     &ciphertextLength, nullptr, 0, bytesOf(request.plaintext),
                     request.plaintext.size()) != 1) {
    outcome.status = PushSealStatus::SealFailed;
    return outcome;
  }
  ciphertext.resize(ciphertextLength);

  envelope.enc = std::move(enc);
  envelope.ciphertext = std::move(ciphertext);
  if (!push_envelope::wellFormed(envelope)) {
    outcome.status = PushSealStatus::SealFailed;
    return outcome;
  }
  outcome.status = PushSealStatus::Sealed;
  outcome.envelope = std::move(envelope);
  return outcome;
}
