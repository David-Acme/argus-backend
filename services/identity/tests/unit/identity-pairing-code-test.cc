#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/pairing/dtos/pairing-dto.hxx>
#include <feature/pairing/infra/pairing-code.hxx>
#include <feature/rate-gate/infra/rate-limiter.hxx>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <errors/validation-exception.hxx>

namespace
{
constexpr const char* kCodeFile = "identity-pairing-code-test.code";

void writeCode(const std::string& code)
{
  std::ofstream out(kCodeFile, std::ios::trunc);
  out << code << '\n';
}
}

TEST_CASE("the pairing code carries 128 bits as QR-friendly base32")
{
  const std::string foobar = "foobar";
  const std::span<const unsigned char> bytes(
      reinterpret_cast<const unsigned char*>(foobar.data()), foobar.size());
  CHECK(pairing_code::encodeBase32(bytes) == "MZXW6YTBOI");

  const auto code = pairing_code::mint();
  REQUIRE(code.has_value());
  CHECK(code->size() == pairing_code::kBase32Length);
  CHECK(pairing_code::wellFormed(*code));
  const auto other = pairing_code::mint();
  REQUIRE(other.has_value());
  CHECK(*code != *other);

  CHECK(pairing_code::wellFormed("A1B2C3D4E5F6"));
  CHECK(pairing_code::isLegacy("A1B2C3D4E5F6"));
  CHECK_FALSE(pairing_code::wellFormed("A1B2C"));
  CHECK_FALSE(pairing_code::wellFormed("ABCDEFGHIJKLMNOPQRSTUVWXY1"));
  CHECK(pairing_code::normalize("kz65-wsi6 qpgv") == "KZ65WSI6QPGV");
}

TEST_CASE("the pairing proofs are HMAC-SHA256 over the normalized code")
{
  CHECK(pairing_code::hmacHex({.code = "key",
                               .message = "The quick brown fox jumps over the lazy dog"}) ==
        "F7BC83F430538424B13298E6AA6FB143EF4D59A14946175997479DBC2D1A3CD8");
  CHECK(pairing_code::sameCode("ABC", "ABC"));
  CHECK_FALSE(pairing_code::sameCode("ABC", "ABD"));
  CHECK_FALSE(pairing_code::sameCode("", ""));
}

TEST_CASE("the store verifies the code and its proofs and rotates it")
{
  const std::string code = "KZ65WSI6QPGV6CRZK26RVVULHI";
  writeCode(code);
  const PairingCodeStore store(kCodeFile);
  REQUIRE(store.current() == code);
  CHECK(store.verifyCode("kz65wsi6qpgv6crzk26rvvulhi"));
  CHECK_FALSE(store.verifyCode("KZ65WSI6QPGV6CRZK26RVVULHA"));

  const std::string nonce(32, 'a');
  const auto proof = pairing_code::hmacHex(
      {.code = code, .message = "argus-pair-client|" + nonce});
  CHECK(store.verifyProof({.nonce = nonce, .proof = proof}));
  CHECK_FALSE(store.verifyProof({.nonce = nonce, .proof = std::string(64, '0')}));
  CHECK(store.serverProof({.nonce = nonce, .caFingerprint = "AB"}) ==
        pairing_code::hmacHex(
            {.code = code, .message = "argus-pair-server|" + nonce + "|AB"}));

  REQUIRE(store.rotate());
  const auto rotated = store.current();
  REQUIRE(rotated.has_value());
  CHECK(*rotated != code);
  CHECK(rotated->size() == pairing_code::kBase32Length);
  CHECK_FALSE(store.verifyCode(code));
  const auto permissions = std::filesystem::status(kCodeFile).permissions();
  CHECK((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
  CHECK((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);
  std::remove(kCodeFile);
  CHECK_FALSE(store.current().has_value());
}

TEST_CASE("the pairing body accepts the long code and refuses anything else")
{
  Json::Value body(Json::objectValue);
  body["code"] = "KZ65WSI6QPGV6CRZK26RVVULHI";
  CHECK_NOTHROW(static_cast<void>(PairingDto::fromJson(body)));
  body["code"] = "A1B2C3D4E5F6";
  CHECK_NOTHROW(static_cast<void>(PairingDto::fromJson(body)));
  body["code"] = "short";
  CHECK_THROWS_AS(static_cast<void>(PairingDto::fromJson(body)), ValidationException);
}

TEST_CASE("the unauthenticated identity routes are rate limited per client")
{
  const auto start = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  RateLimiter limiter({.enabled = true,
                       .windowSeconds = 60,
                       .maxRequests = 2,
                       .lockoutThreshold = 3,
                       .lockoutSeconds = 300});
  CHECK(limiter.admit({.key = "pairing|10.0.0.1", .now = start}));
  CHECK(limiter.admit({.key = "pairing|10.0.0.1", .now = start}));
  CHECK_FALSE(limiter.admit({.key = "pairing|10.0.0.1", .now = start}));
  CHECK(limiter.admit({.key = "pairing|10.0.0.2", .now = start}));
  CHECK(limiter.admit(
      {.key = "pairing|10.0.0.1", .now = start + std::chrono::seconds(61)}));

  const auto later = start + std::chrono::seconds(200);
  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto at = later + std::chrono::seconds(attempt * 40);
    REQUIRE(limiter.admit({.key = "invitation-resolve|10.0.0.3", .now = at}));
    limiter.recordFailure({.key = "invitation-resolve|10.0.0.3", .now = at});
  }
  CHECK_FALSE(limiter.admit(
      {.key = "invitation-resolve|10.0.0.3", .now = later + std::chrono::seconds(120)}));
  CHECK(limiter.admit(
      {.key = "invitation-resolve|10.0.0.3", .now = later + std::chrono::seconds(500)}));

  RateLimiter disabled({.enabled = false,
                        .windowSeconds = 60,
                        .maxRequests = 1,
                        .lockoutThreshold = 1,
                        .lockoutSeconds = 300});
  for (int i = 0; i < 5; ++i)
    CHECK(disabled.admit({.key = "pairing|10.0.0.9", .now = start}));
}
