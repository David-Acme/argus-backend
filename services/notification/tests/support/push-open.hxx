#pragma once

#include <push/push-envelope.hxx>
#include <push/push-kind.hxx>
#include <push/push-priority.hxx>
#include <push/push-provider.hxx>
#include <text/base64.hxx>
#include <text/json-util.hxx>

#include <json/value.h>
#include <openssl/evp.h>
#include <openssl/hpke.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace push_open
{
inline constexpr std::string_view kRecipientPrivateHex =
    "087ab55d9d51ac0548a3b8ba3cc56a1ca29aed592d4ec7f84ebe4b5330c11144";
inline constexpr std::string_view kRecipientPublicHex =
    "5ee54ad74d51ba57c198138c2f690ea8efaa105f4f5ea7c518c9a905cbab7048";

struct PrivateKeyDeleter
{
  void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};

inline std::string fromHex(std::string_view hex)
{
  std::string out;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2)
    out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(index, 2)), nullptr, 16)));
  return out;
}

inline std::string openRaw(const std::string& enc, const std::string& ciphertext, const std::string& info)
{
  const std::string privateKey = fromHex(kRecipientPrivateHex);
  std::unique_ptr<EVP_PKEY, PrivateKeyDeleter> key{EVP_PKEY_new_raw_private_key_ex(
      nullptr, "X25519", nullptr, reinterpret_cast<const unsigned char*>(privateKey.data()),
      privateKey.size())};
  if (!key)
    return {};
  const OSSL_HPKE_SUITE suite{.kem_id = OSSL_HPKE_KEM_ID_X25519,
                              .kdf_id = OSSL_HPKE_KDF_ID_HKDF_SHA256,
                              .aead_id = OSSL_HPKE_AEAD_ID_CHACHA_POLY1305};
  std::unique_ptr<OSSL_HPKE_CTX, void (*)(OSSL_HPKE_CTX*)> context{
      OSSL_HPKE_CTX_new(OSSL_HPKE_MODE_BASE, suite, OSSL_HPKE_ROLE_RECEIVER, nullptr, nullptr),
      OSSL_HPKE_CTX_free};
  if (!context)
    return {};
  if (OSSL_HPKE_decap(context.get(), reinterpret_cast<const unsigned char*>(enc.data()), enc.size(),
                      key.get(), reinterpret_cast<const unsigned char*>(info.data()), info.size()) != 1)
    return {};
  std::string plaintext(ciphertext.size(), '\0');
  std::size_t length = plaintext.size();
  if (OSSL_HPKE_open(context.get(), reinterpret_cast<unsigned char*>(plaintext.data()), &length,
                     nullptr, 0, reinterpret_cast<const unsigned char*>(ciphertext.data()),
                     ciphertext.size()) != 1)
    return {};
  plaintext.resize(length);
  return plaintext;
}

inline std::optional<PushSealedEnvelope> envelopeOf(const Json::Value& payload)
{
  const auto deliveryId = base64::decode(payload["deliveryId"].asString());
  const auto enc = base64::decode(payload["enc"].asString());
  const auto ciphertext = base64::decode(payload["ciphertext"].asString());
  const auto provider = pushProviderFromString(payload["provider"].asString());
  const auto kind = pushKindFromString(payload["kind"].asString());
  const auto priority = pushPriorityFromString(payload["priority"].asString());
  if (!deliveryId || !enc || !ciphertext || !provider || !kind || !priority)
    return std::nullopt;
  return PushSealedEnvelope{.deliveryId = *deliveryId,
                            .provider = *provider,
                            .kind = *kind,
                            .priority = *priority,
                            .ttlSeconds = payload["ttlSeconds"].asUInt(),
                            .collapseId = payload["collapseId"].asString(),
                            .token = payload["token"].asString(),
                            .enc = *enc,
                            .ciphertext = *ciphertext};
}

inline Json::Value plaintextOf(const std::string& outboxPayload, const std::string& keyId)
{
  const Json::Value payload = json_util::fromString(outboxPayload);
  const auto envelope = envelopeOf(payload);
  if (!envelope)
    return Json::Value{};
  const std::string opened = openRaw(envelope->enc, envelope->ciphertext,
                                     push_envelope::hpkeInfo(keyId, *envelope));
  return json_util::fromString(opened);
}
}
