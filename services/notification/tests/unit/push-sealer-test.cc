#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/push-delivery/push-sealer.hxx>
#include <text/base64.hxx>
#include <text/json-util.hxx>

#include <json/value.h>
#include <openssl/evp.h>
#include <openssl/hpke.h>

#include <cstddef>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
constexpr std::string_view kRecipientPrivateHex =
    "087ab55d9d51ac0548a3b8ba3cc56a1ca29aed592d4ec7f84ebe4b5330c11144";
constexpr std::string_view kRecipientPublicHex =
    "5ee54ad74d51ba57c198138c2f690ea8efaa105f4f5ea7c518c9a905cbab7048";
constexpr std::size_t kCiphertextCap = 2048;
constexpr std::size_t kEnvelopeCeiling = 2429;
constexpr std::size_t kBodyCeiling = 4096;
constexpr std::size_t kPessimisticTokenLength = 255;
constexpr std::size_t kRegisteredTokenCap = 255;

std::string fromHex(std::string_view hex)
{
  std::string out;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2)
    out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(index, 2)), nullptr, 16)));
  return out;
}

std::string base64UrlOf(const std::string& bytes)
{
  std::string text = base64::encode(bytes);
  for (char& symbol : text) {
    if (symbol == '+')
      symbol = '-';
    else if (symbol == '/')
      symbol = '_';
  }
  while (!text.empty() && text.back() == '=')
    text.pop_back();
  return text;
}

PushSealRequest requestWith(std::size_t plaintextSize, std::size_t tokenSize)
{
  return {.deliveryId = std::string(16, '\x01'),
          .provider = PushProvider::Fcm,
          .kind = PushKind::Call,
          .priority = PushPriority::High,
          .ttlSeconds = 60,
          .collapseId = "call-0001",
          .token = std::string(tokenSize, 't'),
          .publicKey = base64UrlOf(fromHex(kRecipientPublicHex)),
          .keyId = "test-key-1",
          .plaintext = std::string(plaintextSize, 'p')};
}

struct PrivateKeyDeleter
{
  void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};

using PrivateKeyPtr = std::unique_ptr<EVP_PKEY, PrivateKeyDeleter>;

std::string openRaw(const std::string& enc, const std::string& ciphertext, const std::string& info,
                    const std::string& aad)
{
  const std::string privateKey = fromHex(kRecipientPrivateHex);
  PrivateKeyPtr key{EVP_PKEY_new_raw_private_key_ex(nullptr, "X25519", nullptr,
                                                    reinterpret_cast<const unsigned char*>(privateKey.data()),
                                                    privateKey.size())};
  const OSSL_HPKE_SUITE suite{.kem_id = OSSL_HPKE_KEM_ID_X25519,
                              .kdf_id = OSSL_HPKE_KDF_ID_HKDF_SHA256,
                              .aead_id = OSSL_HPKE_AEAD_ID_CHACHA_POLY1305};
  std::unique_ptr<OSSL_HPKE_CTX, void (*)(OSSL_HPKE_CTX*)> context{
      OSSL_HPKE_CTX_new(OSSL_HPKE_MODE_BASE, suite, OSSL_HPKE_ROLE_RECEIVER, nullptr, nullptr),
      OSSL_HPKE_CTX_free};
  if (!key || !context)
    return {};
  if (OSSL_HPKE_decap(context.get(), reinterpret_cast<const unsigned char*>(enc.data()), enc.size(),
                      key.get(), reinterpret_cast<const unsigned char*>(info.data()), info.size()) != 1)
    return {};
  std::string plaintext(ciphertext.size(), '\0');
  size_t length = plaintext.size();
  if (OSSL_HPKE_open(context.get(), reinterpret_cast<unsigned char*>(plaintext.data()), &length,
                     reinterpret_cast<const unsigned char*>(aad.data()), aad.size(),
                     reinterpret_cast<const unsigned char*>(ciphertext.data()), ciphertext.size()) != 1)
    return {};
  plaintext.resize(length);
  return plaintext;
}

std::string openWith(const PushSealedEnvelope& envelope, const std::string& info)
{
  return openRaw(envelope.enc, envelope.ciphertext, info, {});
}

std::string readFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}
}

TEST_CASE("a sealed push opens only with the envelope prefix bound in its HPKE info")
{
  const PushSealOutcome outcome = PushSealer{}.seal(requestWith(64, 32));
  REQUIRE(outcome.status == PushSealStatus::Sealed);
  const PushSealedEnvelope& envelope = outcome.envelope;
  const std::string info = push_envelope::hpkeInfo("test-key-1", envelope);
  CHECK(openWith(envelope, info) == std::string(64, 'p'));

  PushSealedEnvelope changed = envelope;
  changed.token = std::string(32, 'u');
  CHECK(openWith(envelope, push_envelope::hpkeInfo("test-key-1", changed)).empty());
  CHECK(openWith(envelope, push_envelope::hpkeInfo("test-key-2", envelope)).empty());
}

TEST_CASE("the shared sealing vectors open under their infoHex and refuse a changed info or a non-empty AAD")
{
  const Json::Value document = json_util::fromString(
      readFile(ARGUS_PUSH_SEALED_VECTOR_FILE));
  REQUIRE(document.isObject());
  const Json::Value& cases = document["sealing"]["cases"];
  REQUIRE(cases.isArray());
  CHECK(cases.size() == 5);
  for (const auto& entry : cases) {
    const std::string name = entry["name"].asString();
    const std::string enc = fromHex(entry["encHex"].asString());
    const std::string ciphertext = fromHex(entry["ciphertextHex"].asString());
    const std::string info = fromHex(entry["infoHex"].asString());
    const std::string plaintext = fromHex(entry["plaintextHex"].asString());
    CHECK(openRaw(enc, ciphertext, info, {}) == plaintext);
    std::string changedInfo = info;
    changedInfo.back() = static_cast<char>(changedInfo.back() ^ 0x01);
    CHECK_MESSAGE(openRaw(enc, ciphertext, changedInfo, {}).empty(), name);
    CHECK_MESSAGE(openRaw(enc, ciphertext, info, std::string(1, 'x')).empty(), name);
  }
}

TEST_CASE("a ciphertext at the 2048-byte cap seals and its envelope fits the pessimistic body")
{
  const PushSealOutcome outcome =
      PushSealer{}.seal(requestWith(kCiphertextCap - 16, kPessimisticTokenLength));
  REQUIRE(outcome.status == PushSealStatus::Sealed);
  CHECK(outcome.envelope.ciphertext.size() == kCiphertextCap);

  const auto envelopeBytes = push_envelope::sealedPayload(outcome.envelope);
  REQUIRE(envelopeBytes.has_value());
  CHECK(envelopeBytes->size() <= kEnvelopeCeiling);

  Json::Value body(Json::objectValue);
  body["e"] = base64::encode(*envelopeBytes);
  const std::string serialized = json_util::toString(body) + std::string(kPessimisticTokenLength, 't');
  CHECK(serialized.size() < kBodyCeiling);
  MESSAGE("measured envelope bytes: " << envelopeBytes->size() << "; body bytes: "
                                      << serialized.size());
}

TEST_CASE("a plaintext one byte over the cap is refused with the typed error and nothing is sealed")
{
  const PushSealOutcome outcome =
      PushSealer{}.seal(requestWith(kCiphertextCap - 16 + 1, kPessimisticTokenLength));
  CHECK(outcome.status == PushSealStatus::IntentTooLarge);
  CHECK(outcome.envelope.ciphertext.empty());
  CHECK(outcome.envelope.enc.empty());
}

TEST_CASE("a registered token of 255 bytes seals")
{
  const PushSealOutcome outcome = PushSealer{}.seal(requestWith(64, kRegisteredTokenCap));
  CHECK(outcome.status == PushSealStatus::Sealed);
}

TEST_CASE("a registered token of 256 bytes is refused with token_too_long and nothing is sealed")
{
  const PushSealOutcome outcome = PushSealer{}.seal(requestWith(64, kRegisteredTokenCap + 1));
  CHECK(outcome.status == PushSealStatus::TokenTooLong);
  CHECK(outcome.envelope.ciphertext.empty());
  CHECK(outcome.envelope.enc.empty());
}
