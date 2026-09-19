#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <protocol/frames.hxx>

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

using namespace tunnel;

namespace
{
std::string hex(const std::string& bytes)
{
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (char byte : bytes) {
    const auto value = static_cast<uint8_t>(byte);
    out.push_back(kDigits[value >> 4]);
    out.push_back(kDigits[value & 0x0F]);
  }
  return out;
}
} // namespace

TEST_CASE("frame header layout is little-endian on the wire")
{
  const std::string frame =
      encodeFrame({.type = FrameType::Data, .streamId = 0x0102A3B4, .payload = "xy", .size = 2});
  REQUIRE(frame.size() == kHeaderSize + 2);
  CHECK(frame[0] == static_cast<char>(0x55));
  CHECK(frame[1] == static_cast<char>(0xA7));
  CHECK(frame[2] == static_cast<char>(kVersion));
  CHECK(frame[3] == static_cast<char>(FrameType::Data));
  CHECK(static_cast<uint8_t>(frame[4]) == 0xB4);
  CHECK(static_cast<uint8_t>(frame[5]) == 0xA3);
  CHECK(static_cast<uint8_t>(frame[6]) == 0x02);
  CHECK(static_cast<uint8_t>(frame[7]) == 0x01);
  CHECK(static_cast<uint8_t>(frame[8]) == 0x02);
  CHECK(static_cast<uint8_t>(frame[9]) == 0x00);
  CHECK(static_cast<uint8_t>(frame[10]) == 0x00);
  CHECK(static_cast<uint8_t>(frame[11]) == 0x00);
  CHECK(frame.substr(kHeaderSize) == "xy");
}

TEST_CASE("encode and parse round-trip every frame type")
{
  const FrameType types[] = {
      FrameType::Auth,     FrameType::AuthOk, FrameType::AuthFail,
      FrameType::Open,     FrameType::Data,   FrameType::Close,
      FrameType::Ping,     FrameType::Pong,   FrameType::Push,
      FrameType::Challenge,
  };
  for (const FrameType type : types) {
    FrameParser parser;
    parser.feed(encodeFrame({.type = type, .streamId = 77, .payload = "payload", .size = 7}));
    REQUIRE(parser.hasFrame());
    const Frame frame = parser.popFrame();
    CHECK(frame.type == type);
    CHECK(frame.streamId == 77);
    CHECK(frame.payload == "payload");
    CHECK_FALSE(parser.hasFrame());
    CHECK_FALSE(parser.failed());
  }
}

TEST_CASE("parser reassembles frames fed byte by byte")
{
  const std::string first = encodeFrame(FrameType::Open, 5);
  const std::string second =
      encodeFrame({.type = FrameType::Data, .streamId = 5, .payload = "chunked", .size = 7});
  const std::string wire = first + second;

  FrameParser parser;
  for (char byte : wire)
    parser.feed(&byte, 1);
  REQUIRE(parser.hasFrame());
  Frame frame = parser.popFrame();
  CHECK(frame.type == FrameType::Open);
  CHECK(frame.streamId == 5);
  CHECK(frame.payload.empty());
  REQUIRE(parser.hasFrame());
  frame = parser.popFrame();
  CHECK(frame.type == FrameType::Data);
  CHECK(frame.payload == "chunked");
}

TEST_CASE("parser buffers partial payloads across feeds")
{
  const std::string wire =
      encodeFrame({.type = FrameType::Data, .streamId = 3, .payload = "abcdefgh", .size = 8});
  FrameParser parser;
  parser.feed(wire.data(), kHeaderSize + 3);
  CHECK_FALSE(parser.hasFrame());
  parser.feed(wire.data() + kHeaderSize + 3, 5);
  REQUIRE(parser.hasFrame());
  CHECK(parser.popFrame().payload == "abcdefgh");
}

TEST_CASE("parser rejects oversize, corrupt and unknown frames")
{
  SUBCASE("payload length beyond kMaxPayload")
  {
    FrameParser parser;
    std::string header = encodeFrame(FrameType::Data, 1);
    constexpr size_t kOversizePayload = kMaxPayload + 1;
    header[8] = static_cast<char>(kOversizePayload & 0xFF);
    header[9] = static_cast<char>((kOversizePayload >> 8) & 0xFF);
    header[10] = static_cast<char>((kOversizePayload >> 16) & 0xFF);
    parser.feed(header);
    CHECK(parser.failed());
  }
  SUBCASE("bad magic")
  {
    FrameParser parser;
    std::string header = encodeFrame(FrameType::Ping, 0);
    header[0] = 'X';
    parser.feed(header);
    CHECK(parser.failed());
  }
  SUBCASE("bad version")
  {
    FrameParser parser;
    std::string header = encodeFrame(FrameType::Ping, 0);
    header[2] = 9;
    parser.feed(header);
    CHECK(parser.failed());
  }
  SUBCASE("unknown frame type")
  {
    FrameParser parser;
    std::string header = encodeFrame(FrameType::Ping, 0);
    header[3] = static_cast<char>(kMaxFrameType + 1);
    parser.feed(header);
    CHECK(parser.failed());
  }
  SUBCASE("failed parser stays failed and ignores further input")
  {
    FrameParser parser;
    parser.feed(std::string(kHeaderSize, 'x'));
    CHECK(parser.failed());
    parser.feed(encodeFrame(FrameType::Ping, 0));
    CHECK_FALSE(parser.hasFrame());
  }
}

TEST_CASE("HMAC-SHA256 matches RFC 4231 vectors")
{
  const std::string key1(20, '\x0b');
  CHECK(hex(hmacSha256(key1, "Hi There")) ==
        "b0344c61d8db38535ca8afceaf0bf12b"
        "881dc200c9833da726e9376c2e32cff7");

  const std::string key2 = "Jefe";
  CHECK(hex(hmacSha256(key2, "what do ya want for nothing?")) ==
        "5bdcc146bf60754e6a042426089575c7"
        "5a003f089d2739839dec58b964ec3843");

  // Key longer than the 64-byte block is hashed first.
  const std::string key3(131, '\xaa');
  CHECK(hex(hmacSha256(key3,
                       "Test Using Larger Than Block-Size Key - Hash Key "
                       "First")) ==
        "60e431591ee0b67f0d8a26aacbf5b77f"
        "8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("auth macs are 32 bytes, secret sensitive and challenge bound")
{
  const std::string challenge(kChallengeSize, '\x11');
  const std::string mac = authMac("a-secret", challenge);
  CHECK(mac.size() == kAuthPayloadSize);
  CHECK(mac == authMac("a-secret", challenge));
  CHECK(mac != authMac("another-secret", challenge));
  CHECK(mac != authMac("a-secret", std::string(kChallengeSize, '\x12')));
  CHECK(mac != authMac("a-secret", ""));
  CHECK(mac != relayAuthMac("a-secret", challenge));
}

TEST_CASE("random challenges are 32 bytes and do not repeat")
{
  const std::string first = randomChallenge();
  const std::string second = randomChallenge();
  CHECK(first.size() == kChallengeSize);
  CHECK(second.size() == kChallengeSize);
  CHECK(first != second);
}

TEST_CASE("constant time comparison only accepts equal macs")
{
  const std::string challenge(kChallengeSize, '\x11');
  const std::string mac = authMac("a-secret", challenge);
  CHECK(constantTimeEquals(mac, mac));
  CHECK_FALSE(constantTimeEquals(mac, authMac("other", challenge)));
  CHECK_FALSE(constantTimeEquals(mac, mac.substr(0, 31)));
  CHECK(constantTimeEquals("", ""));
}
