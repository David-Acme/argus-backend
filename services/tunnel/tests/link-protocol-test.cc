#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tls-hello.hxx"

#include <link/protocol/client-hello.hxx>
#include <link/protocol/credit-window.hxx>
#include <link/protocol/frames.hxx>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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

std::filesystem::path cloudLinkVectorDirectory()
{
  const char* cloudDir = std::getenv("ARGUS_CLOUD_DIR");
  if (cloudDir != nullptr && *cloudDir != '\0')
    return std::filesystem::path{cloudDir} / "tests" / "protocol" / "vectors";
  return ARGUS_CLOUD_LINK_VECTOR_DIR;
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

  TEST_CASE("the cloud's vectors are the ones this test pinned")
  {
    const std::filesystem::path cloudDirectory = cloudLinkVectorDirectory();
    if (!std::filesystem::exists(cloudDirectory)) {
      std::cerr << "SKIPPED cloud link vectors: " << cloudDirectory.string() << " missing\n";
      return;
    }
    const nlohmann::json pinned = pins().at("files");
    std::set<std::string> cloud;
    for (const auto& entry : std::filesystem::directory_iterator{cloudDirectory}) {
      if (entry.path().extension() == ".json" &&
          entry.path().filename() != "pins.json")
        cloud.insert(entry.path().filename().string());
    }
    CHECK_FALSE(cloud.empty());
    for (const auto& name : cloud) {
      const std::filesystem::path path = cloudDirectory / name;
      REQUIRE_MESSAGE(pinned.contains(name),
                      "the cloud holds a vector this repository never pinned: "
                          << name);
      CHECK(sha256OfFile(path) == pinned.at(name).get<std::string>());
    }
    CHECK(cloud.size() == pinned.size());
  }
}

namespace
{
using tunnel::link::ClientHelloResult;
using tunnel::link::ClientHelloStatus;

constexpr uint32_t kRandomSeed = 20261013U;
constexpr uint32_t kMutationSeed = 1013U;
constexpr size_t kRandomCases = 4096;
constexpr size_t kMutationCases = 4096;
constexpr uint32_t kMaxRandomLength = 512;
constexpr size_t kRecordHeader = 5;
constexpr size_t kHandshakeHeader = 4;
constexpr size_t kVersionLength = 2;
constexpr size_t kRandomLength = 32;
constexpr size_t kVersionAndRandom = kVersionLength + kRandomLength;
constexpr size_t kExtensionHeader = 4;
constexpr size_t kKeyShareEntryHeader = 4;
constexpr uint32_t kServerNameType = 0;
constexpr uint32_t kKeyShareType = 51;
constexpr std::string_view kBehind = "bytes behind the record";

struct LengthField
{
  size_t offset{0};
  size_t width{0};
};

constexpr LengthField kRecordLength{.offset = 3, .width = 2};
constexpr LengthField kHandshakeLength{.offset = 6, .width = 3};

struct FieldWrite
{
  LengthField field;
  uint32_t value{0};
};

struct LengthChange
{
  uint32_t before{0};
  size_t width{0};
};

struct Extension
{
  uint32_t type{0};
  size_t dataAt{0};
  size_t size{0};
};

struct HelloLayout
{
  std::vector<LengthField> lengths;
  std::vector<Extension> extensions;
  size_t sessionIdAt{0};
  size_t sessionIdSize{0};
};

struct ValidHello
{
  std::string bytes;
  std::string serverName;
};

std::mt19937 seededEngine(uint32_t seed)
{
  std::seed_seq sequence{seed};
  return std::mt19937{sequence};
}

uint32_t below(std::mt19937& engine, uint32_t bound)
{
  return static_cast<uint32_t>(engine() % bound);
}

std::string randomBytes(std::mt19937& engine, size_t count)
{
  std::string bytes(count, '\0');
  std::ranges::generate(bytes, [&engine] { return static_cast<char>(engine() & 0xFFU); });
  return bytes;
}

uint32_t readField(std::string_view bytes, const LengthField& field)
{
  uint32_t value = 0;
  for (const char byte : bytes.substr(field.offset, field.width))
    value = (value << 8U) | static_cast<uint8_t>(byte);
  return value;
}

void writeField(std::string& bytes, const FieldWrite& write)
{
  for (size_t index = 0; index < write.field.width; ++index) {
    const auto shift = static_cast<uint32_t>(8 * (write.field.width - 1 - index));
    bytes.at(write.field.offset + index) =
        static_cast<char>((write.value >> shift) & 0xFFU);
  }
}

uint32_t changedLength(std::mt19937& engine, const LengthChange& change)
{
  const uint32_t mask = (1U << static_cast<uint32_t>(8 * change.width)) - 1U;
  uint32_t after = 0;
  switch (below(engine, 3)) {
  case 0:
    after = static_cast<uint32_t>(engine()) & mask;
    break;
  case 1: {
    const uint32_t delta = 1 + below(engine, 16);
    after = (below(engine, 2) == 0 ? change.before + delta : change.before - delta) & mask;
    break;
  }
  default:
    after = below(engine, 65);
    break;
  }
  return after == change.before ? (after + 1) & mask : after;
}

std::string handshakeRecord(std::string_view body)
{
  std::string record(kRecordHeader + kHandshakeHeader, '\0');
  record[0] = static_cast<char>(0x16);
  record[1] = static_cast<char>(0x03);
  record[2] = static_cast<char>(0x01);
  record[kRecordHeader] = static_cast<char>(0x01);
  writeField(record, {.field = kRecordLength,
                      .value = static_cast<uint32_t>(kHandshakeHeader + body.size())});
  writeField(record,
             {.field = kHandshakeLength, .value = static_cast<uint32_t>(body.size())});
  record.append(body);
  return record;
}

HelloLayout layoutOf(std::string_view record)
{
  size_t cursor = kRecordHeader + kHandshakeHeader + kVersionAndRandom;
  HelloLayout layout{.lengths = {kRecordLength, kHandshakeLength},
                     .extensions = {},
                     .sessionIdAt = cursor + 1,
                     .sessionIdSize = readField(record, {.offset = cursor, .width = 1})};
  for (const size_t width : {size_t{1}, size_t{2}, size_t{1}}) {
    const LengthField field{.offset = cursor, .width = width};
    layout.lengths.push_back(field);
    cursor += width + readField(record, field);
  }
  if (cursor >= record.size())
    return layout;
  layout.lengths.push_back({.offset = cursor, .width = 2});
  cursor += 2;
  while (cursor + kExtensionHeader <= record.size()) {
    const Extension extension{.type = readField(record, {.offset = cursor, .width = 2}),
                              .dataAt = cursor + kExtensionHeader,
                              .size = readField(record, {.offset = cursor + 2, .width = 2})};
    layout.lengths.push_back({.offset = cursor + 2, .width = 2});
    if (extension.type == kServerNameType) {
      layout.lengths.push_back({.offset = extension.dataAt, .width = 2});
      layout.lengths.push_back({.offset = extension.dataAt + 3, .width = 2});
    }
    layout.extensions.push_back(extension);
    cursor = extension.dataAt + extension.size;
  }
  return layout;
}

std::string steadied(std::string hello)
{
  const HelloLayout layout = layoutOf(hello);
  hello.replace(kRecordHeader + kHandshakeHeader + kVersionLength, kRandomLength,
                kRandomLength, '\x11');
  hello.replace(layout.sessionIdAt, layout.sessionIdSize, layout.sessionIdSize, '\x22');
  for (const Extension& extension : layout.extensions) {
    if (extension.type != kKeyShareType)
      continue;
    size_t cursor = extension.dataAt + 2;
    while (cursor + kKeyShareEntryHeader <= extension.dataAt + extension.size) {
      const uint32_t keySize = readField(hello, {.offset = cursor + 2, .width = 2});
      hello.replace(cursor + kKeyShareEntryHeader, keySize, keySize, '\x33');
      cursor += kKeyShareEntryHeader + keySize;
    }
  }
  return hello;
}

std::vector<ValidHello> validHellos()
{
  std::vector<ValidHello> hellos;
  for (const std::string& name :
       {std::string{"sync--0123456789abcdef0123456789abcdef.argus-tunnel.lynk.page"},
        std::string{"0123456789abcdef0123456789abcdef.argus-tunnel.lynk.page"},
        std::string{"notification--fedcba9876543210fedcba9876543210.argus-tunnel.lynk.page"},
        std::string(200, 'x') + ".argus-tunnel.lynk.page", std::string{}}) {
    std::string hello = steadied(tunnel::test::clientHelloFor(name));
    REQUIRE(hello == steadied(tunnel::test::clientHelloFor(name)));
    hellos.push_back({.bytes = std::move(hello), .serverName = name});
  }
  return hellos;
}

ClientHelloResult parseExact(std::string_view bytes)
{
  const std::vector<char> exact(bytes.begin(), bytes.end());
  return tunnel::link::parseClientHello(std::string_view{exact.data(), exact.size()});
}

ClientHelloResult readChecked(std::string_view input)
{
  const ClientHelloResult result = parseExact(input);
  if (result.status == ClientHelloStatus::Parsed)
    CHECK((result.serverName.empty() ||
           input.find(result.serverName) != std::string_view::npos));
  else
    CHECK(result.serverName.empty());
  if (result.status != ClientHelloStatus::NeedMore) {
    const ClientHelloResult followed = parseExact(std::string{input}.append(kBehind));
    CHECK(followed.status == result.status);
    CHECK(followed.serverName == result.serverName);
  }
  return result;
}
}

TEST_SUITE("randomized client hello")
{
  TEST_CASE("random bytes are read or refused, never past their end")
  {
    std::mt19937 engine = seededEngine(kRandomSeed);
    size_t headed = 0;
    for (size_t round = 0; round < kRandomCases; ++round) {
      std::string bytes = randomBytes(engine, below(engine, kMaxRandomLength + 1));
      const bool withHeaders =
          round % 2 == 1 && bytes.size() >= kRecordHeader + kHandshakeHeader;
      if (withHeaders) {
        bytes[0] = static_cast<char>(0x16);
        bytes[kRecordHeader] = static_cast<char>(0x01);
        writeField(bytes, {.field = kRecordLength,
                           .value = static_cast<uint32_t>(bytes.size() - kRecordHeader)});
        writeField(bytes, {.field = kHandshakeLength,
                           .value = static_cast<uint32_t>(bytes.size() - kRecordHeader -
                                                          kHandshakeHeader)});
        ++headed;
      }
      const ClientHelloResult result = readChecked(bytes);
      if (withHeaders)
        CHECK(result.status != ClientHelloStatus::NeedMore);
    }
    MESSAGE(kRandomCases << " random inputs, " << headed
                         << " of them behind whole record and handshake headers");
  }

  TEST_CASE("every truncation of a valid hello waits for the rest")
  {
    size_t truncations = 0;
    for (const ValidHello& valid : validHellos()) {
      for (size_t cut = 0; cut < valid.bytes.size(); ++cut, ++truncations)
        CHECK(readChecked(std::string_view{valid.bytes}.substr(0, cut)).status ==
              ClientHelloStatus::NeedMore);
      const ClientHelloResult whole = readChecked(valid.bytes);
      CHECK(whole.status == ClientHelloStatus::Parsed);
      CHECK(whole.serverName == valid.serverName);
    }
    MESSAGE(truncations << " truncated hellos");
  }

  TEST_CASE("a valid hello whose body is cut short is refused or read without a name")
  {
    size_t cuts = 0;
    for (const ValidHello& valid : validHellos()) {
      const std::string_view body =
          std::string_view{valid.bytes}.substr(kRecordHeader + kHandshakeHeader);
      for (size_t cut = 0; cut < body.size(); ++cut, ++cuts) {
        const ClientHelloResult result = readChecked(handshakeRecord(body.substr(0, cut)));
        CHECK(result.status != ClientHelloStatus::NeedMore);
        CHECK(result.serverName.empty());
      }
    }
    MESSAGE(cuts << " hellos cut inside their body");
  }

  TEST_CASE("a valid hello with one length changed is read or refused, never past its end")
  {
    std::mt19937 engine = seededEngine(kMutationSeed);
    const std::vector<ValidHello> hellos = validHellos();
    for (size_t round = 0; round < kMutationCases; ++round) {
      const ValidHello& valid = hellos[below(engine, static_cast<uint32_t>(hellos.size()))];
      const std::vector<LengthField> fields = layoutOf(valid.bytes).lengths;
      const LengthField field = fields[below(engine, static_cast<uint32_t>(fields.size()))];
      std::string changed = valid.bytes;
      writeField(changed,
                 {.field = field,
                  .value = changedLength(engine, {.before = readField(valid.bytes, field),
                                                  .width = field.width})});
      readChecked(changed);
    }
    MESSAGE(kMutationCases << " hellos with one length changed");
  }
}
