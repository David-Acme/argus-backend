#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tls-hello.hxx"

#include <link/protocol/client-hello.hxx>
#include <link/protocol/credit-window.hxx>
#include <link/protocol/frames.hxx>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

namespace
{
using tunnel::link::CloseReason;
using tunnel::link::Frame;
using tunnel::link::FrameParser;
using tunnel::link::FrameType;
using tunnel::link::WireError;

std::string bytesOfHex(std::string_view hex)
{
  REQUIRE(hex.size() % 2 == 0);
  std::string bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t index = 0; index < hex.size(); index += 2) {
    bytes.push_back(static_cast<char>(
        std::stoul(std::string{hex.substr(index, 2)}, nullptr, 16)));
  }
  return bytes;
}

constexpr std::string_view kHexDigits = "0123456789abcdef";

std::string hexOf(std::string_view bytes)
{
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const char byte : bytes) {
    const auto value = static_cast<uint32_t>(static_cast<uint8_t>(byte));
    hex.push_back(kHexDigits[value >> 4U]);
    hex.push_back(kHexDigits[value & 0x0FU]);
  }
  return hex;
}

std::string sha256OfFile(const std::filesystem::path& path)
{
  std::ifstream stream{path, std::ios::binary};
  REQUIRE(stream.good());
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  REQUIRE(context != nullptr);
  EVP_DigestInit_ex(context, EVP_sha256(), nullptr);
  std::array<char, 4096> chunk{};
  while (stream.good()) {
    stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto count = stream.gcount();
    if (count > 0)
      EVP_DigestUpdate(context, chunk.data(), static_cast<size_t>(count));
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  EVP_DigestFinal_ex(context, digest.data(), &length);
  EVP_MD_CTX_free(context);
  std::string hex;
  hex.reserve(static_cast<size_t>(length) * 2);
  for (unsigned int index = 0; index < length; ++index) {
    const auto value = static_cast<uint32_t>(digest[index]);
    hex.push_back(kHexDigits[value >> 4U]);
    hex.push_back(kHexDigits[value & 0x0FU]);
  }
  return hex;
}

nlohmann::json loadFile(const std::filesystem::path& path)
{
  std::ifstream stream{path};
  REQUIRE(stream.good());
  nlohmann::json document;
  stream >> document;
  return document;
}

std::vector<std::filesystem::path> vectorFiles()
{
  std::vector<std::filesystem::path> files;
  for (const auto& entry :
       std::filesystem::directory_iterator{ARGUS_LINK_VECTOR_DIR}) {
    if (entry.path().extension() == ".json" &&
        entry.path().filename() != "pins.json")
      files.push_back(entry.path());
  }
  std::ranges::sort(files);
  REQUIRE(!files.empty());
  return files;
}

nlohmann::json pins()
{
  return loadFile(std::filesystem::path{ARGUS_LINK_VECTOR_DIR} / "pins.json");
}

bool cloudVectorsAvailable()
{
  return std::filesystem::exists(ARGUS_CLOUD_LINK_VECTOR_DIR);
}

std::string kindOf(const nlohmann::json& vector)
{
  return vector.at("type").get<std::string>();
}

Frame frameOf(std::string_view kind, const nlohmann::json& vector)
{
  const auto type = tunnel::link::frameTypeFromString(kind);
  REQUIRE(type.has_value());
  Frame frame;
  frame.type = type.value_or(FrameType::Data);
  frame.streamId = vector.at("streamId").get<uint32_t>();
  frame.payload = bytesOfHex(vector.at("payloadHex").get<std::string>());
  return frame;
}

std::string encodedFrameOf(std::string_view kind, const nlohmann::json& vector)
{
  const Frame frame = frameOf(kind, vector);
  return tunnel::link::encodeFrame({.type = frame.type,
                                    .streamId = frame.streamId,
                                    .payload = frame.payload});
}
}

TEST_SUITE("home link v2 vectors")
{
  TEST_CASE("every frame vector encodes and decodes")
  {
    size_t checked = 0;
    for (const auto& file : vectorFiles()) {
      const auto document = loadFile(file);
      if (!document.contains("frame"))
        continue;
      const std::string kind = document.at("frame").get<std::string>();
      for (const auto& vector : document.at("vectors")) {
        const std::string expected = vector.at("bytesHex").get<std::string>();
        CHECK(hexOf(encodedFrameOf(kind, vector)) == expected);

        FrameParser parser;
        parser.feed(bytesOfHex(expected));
        REQUIRE_FALSE(parser.failed());
        REQUIRE(parser.hasFrame());
        const Frame decoded = parser.popFrame();
        const Frame declared = frameOf(kind, vector);
        CHECK(decoded.type == declared.type);
        CHECK(decoded.streamId == declared.streamId);
        CHECK(decoded.payload == declared.payload);
        CHECK_FALSE(parser.hasFrame());
        ++checked;
      }
    }
    CHECK(checked >= 15);
  }

  TEST_CASE("every refusal vector is refused with its own spelling")
  {
    size_t checked = 0;
    const nlohmann::json document =
        loadFile(std::filesystem::path{ARGUS_LINK_VECTOR_DIR} /
                 "refusals.json");
    for (const auto& vector : document.at("vectors")) {
      if (!vector.contains("error"))
        continue;
      const auto expected = tunnel::link::wireErrorFromString(
          vector.at("error").get<std::string>());
      REQUIRE(expected.has_value());
      FrameParser parser;
      parser.feed(bytesOfHex(vector.at("bytesHex").get<std::string>()));
      CHECK(parser.failed());
      CHECK(parser.error() == expected.value_or(WireError::BadMagic));
      CHECK_FALSE(parser.hasFrame());
      parser.feed(bytesOfHex(vector.at("bytesHex").get<std::string>()));
      CHECK(parser.failed());
      ++checked;
    }
    CHECK(checked >= 25);
  }

  TEST_CASE("every incomplete vector waits for the rest of its frame")
  {
    size_t checked = 0;
    const nlohmann::json document =
        loadFile(std::filesystem::path{ARGUS_LINK_VECTOR_DIR} /
                 "refusals.json");
    for (const auto& vector : document.at("vectors")) {
      if (vector.value("expect", "") != "incomplete")
        continue;
      FrameParser parser;
      parser.feed(bytesOfHex(vector.at("bytesHex").get<std::string>()));
      CHECK_FALSE(parser.failed());
      CHECK_FALSE(parser.hasFrame());
      const std::string whole = encodedFrameOf(kindOf(vector), vector);
      const std::string part =
          bytesOfHex(vector.at("bytesHex").get<std::string>());
      REQUIRE(whole.starts_with(part));
      parser.feed(std::string_view{whole}.substr(part.size()));
      REQUIRE_FALSE(parser.failed());
      REQUIRE(parser.hasFrame());
      const Frame decoded = parser.popFrame();
      const Frame declared = frameOf(kindOf(vector), vector);
      CHECK(decoded.type == declared.type);
      CHECK(decoded.streamId == declared.streamId);
      CHECK(decoded.payload == declared.payload);
      ++checked;
    }
    CHECK(checked >= 4);
  }

  TEST_CASE("the flow vectors agree with the credit window")
  {
    const nlohmann::json document =
        loadFile(std::filesystem::path{ARGUS_LINK_VECTOR_DIR} / "flow.json");
    for (const auto& vector : document.at("vectors")) {
      const uint32_t granted = vector.at("grantedCredit").get<uint32_t>();
      tunnel::link::CreditWindow window{
          {.limited = true, .window = granted, .grantThreshold = 1}};
      bool refused = false;
      for (const auto& hex : vector.at("framesHex")) {
        FrameParser parser;
        parser.feed(bytesOfHex(hex.get<std::string>()));
        REQUIRE_FALSE(parser.failed());
        REQUIRE(parser.hasFrame());
        const Frame frame = parser.popFrame();
        if (frame.type == FrameType::Credit) {
          window.grant(tunnel::link::decodeCredit(frame));
          continue;
        }
        if (frame.type != FrameType::Data)
          continue;
        if (!window.covers(frame.payload.size())) {
          refused = true;
          continue;
        }
        window.recordUsed(frame.payload.size());
      }
      const bool expected =
          vector.at("expect").get<std::string>() == "backpressure";
      CHECK(refused == expected);
    }
  }

  TEST_CASE("the handshake vectors are frames this parser accepts")
  {
    size_t checked = 0;
    const nlohmann::json document = loadFile(
        std::filesystem::path{ARGUS_LINK_VECTOR_DIR} / "handshake.json");
    for (const auto& vector : document.at("vectors")) {
      const auto expected = tunnel::link::wireErrorFromString(
          vector.at("error").get<std::string>());
      REQUIRE(expected.has_value());
      for (const auto& hex : vector.at("framesHex")) {
        FrameParser parser;
        parser.feed(bytesOfHex(hex.get<std::string>()));
        REQUIRE_FALSE(parser.failed());
        CHECK(parser.hasFrame());
        (void)parser.popFrame();
        ++checked;
      }
    }
    CHECK(checked >= 4);
  }

  TEST_CASE("the wire spellings hold in both directions")
  {
    for (uint8_t raw = 1; raw <= 9; ++raw) {
      const auto type = static_cast<FrameType>(raw);
      const auto spelling = tunnel::link::frameTypeToString(type);
      const auto parsed = tunnel::link::frameTypeFromString(spelling);
      REQUIRE(parsed.has_value());
      CHECK(parsed.value_or(FrameType::Hello) == type);
    }
    CHECK_FALSE(
        tunnel::link::frameTypeFromString("auth").has_value());
    CHECK_FALSE(tunnel::link::frameTypeFromString("").has_value());

    for (uint8_t raw = 0; raw <= 4; ++raw) {
      const auto reason = static_cast<CloseReason>(raw);
      const auto spelling = tunnel::link::closeReasonToString(reason);
      const auto parsed = tunnel::link::closeReasonFromString(spelling);
      REQUIRE(parsed.has_value());
      CHECK(parsed.value_or(CloseReason::Normal) == reason);
    }
    CHECK_FALSE(tunnel::link::closeReasonFromString("busy_").has_value());

    for (uint16_t raw = 0; raw <= 2; ++raw) {
      const auto reason = static_cast<tunnel::link::GoAwayReason>(raw);
      const auto spelling = tunnel::link::goAwayReasonToString(reason);
      const auto parsed = tunnel::link::goAwayReasonFromString(spelling);
      REQUIRE(parsed.has_value());
      CHECK(parsed.value_or(tunnel::link::GoAwayReason::NodeDrain) == reason);
    }

    for (uint8_t raw = 0; raw < 20; ++raw) {
      const auto error = static_cast<WireError>(raw);
      const auto spelling = tunnel::link::wireErrorToString(error);
      CHECK(spelling != "unknown");
      const auto parsed = tunnel::link::wireErrorFromString(spelling);
      REQUIRE(parsed.has_value());
      CHECK(parsed.value_or(WireError::BadMagic) == error);
    }
    CHECK_FALSE(tunnel::link::wireErrorFromString("bad_").has_value());
  }

  TEST_CASE("a stream that is told to stop sending obeys a finite window")
  {
    tunnel::link::CreditWindow sender{
        {.limited = true, .window = 1024, .grantThreshold = 1}};
    CHECK(sender.covers(1024));
    sender.recordUsed(1024);
    CHECK_FALSE(sender.covers(1));
    sender.grant(1024);
    CHECK(sender.covers(1024));
    sender.grant(4096);
    CHECK(sender.remaining() == 1024);

    tunnel::link::CreditWindow receiver{
        {.limited = true, .window = 4096, .grantThreshold = 1024}};
    receiver.recordUsed(1024);
    receiver.markConsumed(1024);
    CHECK(receiver.takePendingGrant() == 1024);
    CHECK(receiver.takePendingGrant() == 0);

    tunnel::link::CreditWindow unlimited{
        {.limited = false, .window = 0, .grantThreshold = 1}};
    CHECK(unlimited.covers(static_cast<size_t>(1024) * 1024));
    unlimited.recordUsed(static_cast<size_t>(1024) * 1024);
    CHECK(unlimited.covers(static_cast<size_t>(1024) * 1024));
    CHECK(unlimited.takePendingGrant() == 0);
  }

  TEST_CASE("the local vectors match their pins")
  {
    const nlohmann::json document = pins();
    CHECK(document.at("algorithm").get<std::string>() == "sha256");
    const auto& files = document.at("files");
    std::set<std::string> listed;
    for (const auto& [name, _] : files.items()) {
      const std::filesystem::path path =
          std::filesystem::path{ARGUS_LINK_VECTOR_DIR} / name;
      REQUIRE(std::filesystem::exists(path));
      CHECK(sha256OfFile(path) == files.at(name).get<std::string>());
      listed.insert(name);
    }
    for (const auto& file : vectorFiles())
      CHECK(listed.contains(file.filename().string()));
    CHECK(listed.contains("device-names.json"));
    CHECK(listed.size() >= 13);
  }

  TEST_CASE("every device-name vector reads as it says")
  {
    const nlohmann::json document = loadFile(
        std::filesystem::path{ARGUS_LINK_VECTOR_DIR} / "device-names.json");
    const std::string suffix = document.at("domainSuffix").get<std::string>();
    size_t parsed = 0;
    size_t refused = 0;
    for (const auto& vector : document.at("vectors")) {
      const std::string serverName = vector.at("serverName").get<std::string>();
      const std::string label = vector.at("name").get<std::string>();
      INFO(label);
      const auto name = tunnel::link::parseDeviceName(
          {.serverName = serverName, .domainSuffix = suffix});
      if (vector.value("expect", "") == "refused") {
        CHECK_FALSE(name.has_value());
        ++refused;
        continue;
      }
      const tunnel::link::DeviceName actual = name.value_or(
          tunnel::link::DeviceName{.service = "refused", .homeId = "refused"});
      CHECK(actual.service == vector.at("service").get<std::string>());
      CHECK(actual.homeId == vector.at("homeId").get<std::string>());
      ++parsed;
    }
    CHECK(parsed >= 6);
    CHECK(refused >= 22);
  }

  TEST_CASE("the server name comes out of a client hello, whole or in pieces")
  {
    const std::string name =
        "auth--0123456789abcdef0123456789abcdef.argus-tunnel.lynk.page";
    const std::string hello = tunnel::test::clientHelloFor(name);
    REQUIRE(hello.size() > 5);
    const tunnel::link::ClientHelloResult whole =
        tunnel::link::parseClientHello(hello);
    CHECK(whole.status == tunnel::link::ClientHelloStatus::Parsed);
    CHECK(whole.serverName == name);

    const tunnel::link::ClientHelloResult behind =
        tunnel::link::parseClientHello(hello + "carried bytes behind it");
    CHECK(behind.status == tunnel::link::ClientHelloStatus::Parsed);
    CHECK(behind.serverName == name);

    for (size_t cut = 0; cut < hello.size(); cut += 97)
      CHECK(tunnel::link::parseClientHello(std::string_view{hello}.substr(0, cut))
                .status == tunnel::link::ClientHelloStatus::NeedMore);

    const tunnel::link::ClientHelloResult unnamed =
        tunnel::link::parseClientHello(tunnel::test::clientHelloFor(""));
    CHECK(unnamed.status == tunnel::link::ClientHelloStatus::Parsed);
    CHECK(unnamed.serverName.empty());

    std::string notHandshake = hello;
    notHandshake[0] = static_cast<char>(0x17);
    CHECK(tunnel::link::parseClientHello(notHandshake).status ==
          tunnel::link::ClientHelloStatus::Malformed);
    CHECK(tunnel::link::parseClientHello("GET / HTTP/1.1\r\n\r\n").status ==
          tunnel::link::ClientHelloStatus::Malformed);
  }

  TEST_CASE("a refusal is one unrecognized_name alert record")
  {
    CHECK(hexOf(tunnel::link::tlsAlertRecord(
              tunnel::link::kUnrecognizedNameAlert)) == "15030300020270");
  }

  TEST_CASE("the cloud's vectors are the ones this test pinned"
            * doctest::skip(!cloudVectorsAvailable()))
  {
    const nlohmann::json pinned = pins().at("files");
    std::set<std::string> cloud;
    for (const auto& entry :
         std::filesystem::directory_iterator{ARGUS_CLOUD_LINK_VECTOR_DIR}) {
      if (entry.path().extension() == ".json" &&
          entry.path().filename() != "pins.json")
        cloud.insert(entry.path().filename().string());
    }
    CHECK_FALSE(cloud.empty());
    for (const auto& name : cloud) {
      const std::filesystem::path path =
          std::filesystem::path{ARGUS_CLOUD_LINK_VECTOR_DIR} / name;
      REQUIRE_MESSAGE(pinned.contains(name),
                      "the cloud holds a vector this repository never pinned: "
                          << name);
      CHECK(sha256OfFile(path) == pinned.at(name).get<std::string>());
    }
    CHECK(cloud.size() == pinned.size());
  }
}
