#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "link-test-cert.hxx"

#include <link/client/link-client.hxx>
#include <link/protocol/frames.hxx>
#include <link/push/sealed-push-bridge.hxx>
#include <net/poll-loop.hxx>
#include <push/push-envelope.hxx>
#include <text/base64.hxx>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
namespace fs = std::filesystem;
using tunnel::link::FrameType;

constexpr std::string_view kPinnedSha256 =
    "1220224c85fbec191b4398c9c11d47f6b7bad8c5242fd9b68db21c69a132e470";
constexpr std::size_t kHeader = 12;

std::string readFile(const fs::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

fs::path cloudSealedPushVectorFile()
{
  const char* cloudDir = std::getenv("ARGUS_CLOUD_DIR");
  if (cloudDir != nullptr && *cloudDir != '\0')
    return fs::path{cloudDir} / "tests" / "vectors" / "push-sealed.json";
  return ARGUS_CLOUD_SEALED_PUSH_VECTOR_FILE;
}

nlohmann::json loadVectors()
{
  return nlohmann::json::parse(readFile(ARGUS_SEALED_PUSH_VECTOR_FILE));
}

std::string fromHex(std::string_view hex)
{
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2)
    out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(index, 2)), nullptr, 16)));
  return out;
}

std::string toHex(std::string_view bytes)
{
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const char byte : bytes) {
    const auto value = static_cast<unsigned char>(byte);
    out.push_back(kDigits[value >> 4U]);
    out.push_back(kDigits[value & 0x0FU]);
  }
  return out;
}

std::string sha256Hex(const std::string& bytes)
{
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr);
  return toHex(std::string_view(reinterpret_cast<const char*>(digest), length));
}

bool isLowerHex(std::string_view text)
{
  return std::ranges::all_of(text, [](char symbol) {
    return (symbol >= '0' && symbol <= '9') || (symbol >= 'a' && symbol <= 'f');
  });
}

bool isApnsProvider(std::string_view provider)
{
  return provider == "apns" || provider == "apns_voip";
}

PushSealedEnvelope fcmEnvelope()
{
  return {.deliveryId = fromHex("000102030405060708090a0b0c0d0e0f"),
          .provider = PushProvider::Fcm,
          .kind = PushKind::Call,
          .priority = PushPriority::High,
          .ttlSeconds = 60,
          .collapseId = "call-0001",
          .token = "fcm-test-token-0001",
          .enc = fromHex("3ace5bee03a93f764e89574e2bb48dcb9a7e136f52225f7788224082496e981c"),
          .ciphertext = std::string(164, '\x05')};
}
}

TEST_CASE("the sealed push vectors are the bytes the cloud pinned")
{
  CHECK(sha256Hex(readFile(ARGUS_SEALED_PUSH_VECTOR_FILE)) == kPinnedSha256);
  const fs::path cloudFile = cloudSealedPushVectorFile();
  if (fs::exists(cloudFile)) {
    CHECK(sha256Hex(readFile(cloudFile)) == kPinnedSha256);
  } else {
    std::cerr << "SKIPPED cloud copy: " << cloudFile.string() << " missing\n";
  }
}

TEST_CASE("every sealed push vector parses to its fields and encodes back to its bytes")
{
  const nlohmann::json document = loadVectors();
  std::size_t checked = 0;
  for (const auto& vector : document.at("vectors")) {
    if (vector.at("frame") != "push_sealed")
      continue;
    ++checked;
    const std::string payload = fromHex(vector.at("payloadHex").get<std::string>());
    PushSealedEnvelope parsed;
    REQUIRE(push_envelope::parseSealedPayload(payload, parsed));
    const auto& fields = vector.at("fields");
    CHECK(toHex(parsed.deliveryId) == fields.at("deliveryIdHex").get<std::string>());
    CHECK(pushProviderToString(parsed.provider) == fields.at("provider").get<std::string>());
    CHECK(pushKindToString(parsed.kind) == fields.at("kind").get<std::string>());
    CHECK(pushPriorityToString(parsed.priority) == fields.at("priority").get<std::string>());
    CHECK(parsed.ttlSeconds == fields.at("ttlSeconds").get<uint32_t>());
    CHECK(parsed.collapseId == fields.at("collapseId").get<std::string>());
    CHECK(parsed.token == fields.at("token").get<std::string>());
    CHECK(toHex(parsed.enc) == fields.at("encHex").get<std::string>());
    CHECK(toHex(parsed.ciphertext) == fields.at("ciphertextHex").get<std::string>());
    const auto encoded = push_envelope::sealedPayload(parsed);
    REQUIRE(encoded.has_value());
    CHECK(toHex(*encoded) == vector.at("payloadHex").get<std::string>());
    const std::string frame = tunnel::link::encodeFrame(
        {.type = FrameType::PushSealed, .streamId = 0, .payload = *encoded});
    CHECK(toHex(frame) == vector.at("bytesHex").get<std::string>());
  }
  CHECK(checked == 5);
}

TEST_CASE("an APNs token travels as its 64 lowercase hex characters, never its 32 raw bytes")
{
  const nlohmann::json document = loadVectors();
  std::size_t checked = 0;
  for (const auto& vector : document.at("vectors")) {
    if (vector.at("frame") != "push_sealed")
      continue;
    const auto& fields = vector.at("fields");
    const std::string token = fields.at("token").get<std::string>();
    if (!isApnsProvider(fields.at("provider").get<std::string>()))
      continue;
    ++checked;
    PushSealedEnvelope parsed;
    REQUIRE(push_envelope::parseSealedPayload(
        fromHex(vector.at("payloadHex").get<std::string>()), parsed));
    CHECK(parsed.token == token);
    CHECK(parsed.token.size() == 64);
    CHECK(isLowerHex(parsed.token));
    CHECK(fromHex(token).size() == 32);
    CHECK(parsed.token.size() != fromHex(token).size());
  }
  CHECK(checked == 2);
}

TEST_CASE("every refused sealed push vector is refused by the payload parser")
{
  const nlohmann::json document = loadVectors();
  std::size_t checked = 0;
  for (const auto& vector : document.at("refusals")) {
    if (vector.at("frame") != "push_sealed")
      continue;
    ++checked;
    PushSealedEnvelope parsed;
    CHECK_FALSE(push_envelope::parseSealedPayload(
        fromHex(vector.at("bytesHex").get<std::string>()).substr(kHeader), parsed));
    CHECK(vector.at("error").get<std::string>() == "protocol_error");
  }
  CHECK(checked == 14);
}

TEST_CASE("every push result vector decodes and every refused one is refused")
{
  const nlohmann::json document = loadVectors();
  std::size_t accepted = 0;
  std::size_t refused = 0;
  for (const auto& vector : document.at("vectors")) {
    if (vector.at("frame") != "push_result")
      continue;
    ++accepted;
    PushResultEnvelope result;
    REQUIRE(push_envelope::parseResultPayload(
        fromHex(vector.at("payloadHex").get<std::string>()), result));
    CHECK(pushOutcomeToString(result.outcome) == vector.at("fields").at("outcome").get<std::string>());
    CHECK(toHex(result.deliveryId) == vector.at("fields").at("deliveryIdHex").get<std::string>());
  }
  for (const auto& vector : document.at("refusals")) {
    if (vector.at("frame") != "push_result")
      continue;
    ++refused;
    PushResultEnvelope result;
    CHECK_FALSE(push_envelope::parseResultPayload(
        fromHex(vector.at("bytesHex").get<std::string>()).substr(kHeader), result));
  }
  CHECK(accepted == 6);
  CHECK(refused == 3);
}

TEST_CASE("a home does not seal a ciphertext over the 2048-byte cap")
{
  PushSealedEnvelope envelope = fcmEnvelope();
  envelope.ciphertext.assign(push_envelope::kMaxCiphertextSize, '\x05');
  CHECK(push_envelope::sealedPayload(envelope).has_value());
  envelope.ciphertext.push_back('\x05');
  CHECK_FALSE(push_envelope::sealedPayload(envelope).has_value());
}

TEST_CASE("a sealed push is not sent before the link negotiates sealed_push")
{
  const auto directory = tunnel::test::makeTempDirectory("argus-tunnel-sealed");
  const auto home = tunnel::test::mintIdentity(
      {.directory = directory, .name = "home", .commonName = "home.argus-sim.test"});
  PollLoop loop;
  tunnel::link::LinkClient client(
      {.loop = loop,
       .options = {.certificatePath = home.certificatePath, .keyPath = home.keyPath},
       .observer = nullptr});
  CHECK_FALSE(client.sealedPushReady());
  CHECK_FALSE(client.sendSealedPush(fcmEnvelope()));
}

TEST_CASE("the frame parser reads push_sealed and push_result as control frames of stream zero")
{
  CHECK(tunnel::link::frameTypeToString(FrameType::PushSealed) == "push_sealed");
  CHECK(tunnel::link::frameTypeFromString("push_result") == FrameType::PushResult);
  const std::string frame = tunnel::link::encodeFrame(
      {.type = FrameType::PushResult, .streamId = 0,
       .payload = std::string(push_envelope::kResultPayloadSize, '\x00')});
  tunnel::link::FrameParser parser;
  parser.feed(frame);
  REQUIRE(parser.hasFrame());
  CHECK(parser.popFrame().type == FrameType::PushResult);
  CHECK_FALSE(parser.failed());
}

TEST_CASE("the NATS bridge reads a delivery and writes a result")
{
  const PushSealedEnvelope envelope = fcmEnvelope();
  const nlohmann::json delivery = {
      {"deliveryId", base64::encode(envelope.deliveryId)},
      {"provider", "fcm"},
      {"kind", "call"},
      {"priority", "high"},
      {"ttlSeconds", 60},
      {"collapseId", "call-0001"},
      {"token", "fcm-test-token-0001"},
      {"enc", base64::encode(envelope.enc)},
      {"ciphertext", base64::encode(envelope.ciphertext)}};
  const auto parsed = tunnel::link::sealed_push_json::deliveryFromJson(delivery.dump());
  REQUIRE(parsed.has_value());
  CHECK(parsed->token == "fcm-test-token-0001");
  CHECK(parsed->ciphertext == envelope.ciphertext);
  CHECK(parsed->provider == PushProvider::Fcm);

  nlohmann::json badKind = delivery;
  badKind["kind"] = "ring";
  CHECK_FALSE(tunnel::link::sealed_push_json::deliveryFromJson(badKind.dump()).has_value());

  const PushResultEnvelope result{.deliveryId = envelope.deliveryId,
                                  .outcome = PushOutcome::InvalidToken};
  const auto written = nlohmann::json::parse(
      tunnel::link::sealed_push_json::resultToJson(result, "fcm-test-token-0001"));
  CHECK(written.at("outcome").get<std::string>() == "invalid_token");
  CHECK(written.at("token").get<std::string>() == "fcm-test-token-0001");
  CHECK(written.at("deliveryId").get<std::string>() == base64::encode(envelope.deliveryId));
}

TEST_CASE("the ledger keeps the token of each delivery until its result or its eviction")
{
  tunnel::link::SealedPushLedger ledger(2);
  ledger.remember("a", "token-a");
  ledger.remember("b", "token-b");
  ledger.remember("c", "token-c");
  CHECK(ledger.take("a").empty());
  CHECK(ledger.take("c") == "token-c");
  CHECK(ledger.take("c").empty());
  CHECK(ledger.take("b") == "token-b");
}
