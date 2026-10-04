#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-talk-channel.hxx"

#include <shared/services/tapo/tapo-crypto.hxx>
#include <shared/services/tapo/tapo-talk-client.hxx>

#include <chrono>
#include <cmath>
#include <span>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using fake_talk::FakeTalkChannel;
using fake_talk::kCloudPassword;

namespace
{

TapoTalkConfig clientConfig(int port)
{
  TapoTalkConfig config;
  config.host = "127.0.0.1";
  config.port = port;
  config.username = "admin";
  config.cloudPassword = kCloudPassword;
  config.connectTimeoutMs = 2000;
  config.ioTimeoutMs = 2000;
  config.pace = false;
  return config;
}

}

TEST_CASE("the talk channel answers the digest challenge on the same connection")
{
  FakeTalkChannel channel(FakeTalkChannel::Mode::AcceptReference);
  TapoTalkClient client(clientConfig(channel.port()));
  const auto opened = client.open();
  REQUIRE(opened.ok);
  CHECK(channel.connections() == 1);
  CHECK(client.state()["passwordVariant"].asString() == "sha256");
  CHECK(client.state()["keyExchangeNonce"].asString() ==
        channel.keyExchangeNonce());
  CHECK(channel.answeredAuthorization().find("username=\"admin\"") !=
        std::string::npos);
  CHECK(channel.answeredAuthorization().find(
            "nonce=\"" + channel.challengeNonce() + "\"") != std::string::npos);
  CHECK(channel.answeredAuthorization().find("nc=00000001") !=
        std::string::npos);
  CHECK(channel.sessionBody().find("\"talk\"") != std::string::npos);
  CHECK(channel.sessionBody().find("\"aec\"") != std::string::npos);
  CHECK(client.state()["sessionId"].asString() == "31415");
  CHECK(client.isOpen());
  client.close();
  CHECK_FALSE(client.isOpen());
  for (int i = 0; i < 400 && channel.stopPlaintext().empty(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(channel.stopEncrypted());
  const std::string stop = channel.stopPlaintext();
  CHECK(stop.find("\"stop\":\"null\"") != std::string::npos);
  CHECK(stop.find("\"method\":\"do\"") != std::string::npos);
  CHECK(stop.find("\"type\":\"request\"") != std::string::npos);
}

TEST_CASE("the talk channel uses the md5 digest when the camera does not ask for sha256")
{
  FakeTalkChannel channel(FakeTalkChannel::Mode::AcceptReference, "0");
  TapoTalkClient client(clientConfig(channel.port()));
  const auto opened = client.open();
  REQUIRE(opened.ok);
  CHECK(channel.connections() == 1);
  CHECK(client.state()["passwordVariant"].asString() == "md5");
  CHECK(client.state()["sessionId"].asString() == "31415");
  client.close();
}

TEST_CASE("the talk channel retries the other derivation before giving up")
{
  FakeTalkChannel channel(FakeTalkChannel::Mode::RejectAll);
  TapoTalkClient client(clientConfig(channel.port()));
  const auto opened = client.open();
  REQUIRE_FALSE(opened.ok);
  CHECK(opened.error.find("digest rejected") != std::string::npos);
  CHECK(opened.error.find("md5") != std::string::npos);
  CHECK(channel.connections() == 2);
  CHECK_FALSE(client.isOpen());
}

TEST_CASE("the talk channel opens without authentication when allowed")
{
  FakeTalkChannel channel(FakeTalkChannel::Mode::Authless);
  TapoTalkClient client(clientConfig(channel.port()));
  const auto opened = client.open();
  REQUIRE(opened.ok);
  CHECK(channel.connections() == 1);
  CHECK(client.state()["passwordVariant"].asString() == "none");
  CHECK(client.state()["sessionId"].asString() == "31415");
  client.close();
}
TEST_CASE("live packets reach the camera as MPEG-TS parts on the open session")
{
  FakeTalkChannel channel(FakeTalkChannel::Mode::AcceptReference);
  TapoTalkClient client(clientConfig(channel.port()));
  REQUIRE(client.open().ok);

  TapoVoiceEncoder encoder;
  std::vector<int16_t> tone(960);
  for (size_t i = 0; i < tone.size(); ++i)
    tone[i] = static_cast<int16_t>((i % 16 < 8) ? 6000 : -6000);
  for (int packet = 0; packet < 3; ++packet) {
    const auto alaw = encoder.encode(tone);
    REQUIRE(alaw.size() == 960);
    REQUIRE(client.sendPacket(alaw).ok);
  }
  CHECK(client.sentDurationMs() == 360);
  client.close();

  for (int i = 0; i < 400 && channel.stopPlaintext().empty(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const auto parts = channel.audioParts();
  REQUIRE(parts.size() == 3);
  for (const auto& part : parts) {
    CHECK(part.size() % 188 == 0);
    CHECK(static_cast<uint8_t>(part[0]) == 0x47);
  }
  for (const auto& id : channel.audioSessionIds())
    CHECK(id == "31415");
  CHECK(channel.stopPlaintext().find("\"stop\":\"null\"") != std::string::npos);
}

TEST_CASE("a packet on a closed channel is refused instead of written")
{
  TapoTalkClient client(clientConfig(1));
  const std::vector<uint8_t> alaw(960, 0xD5);
  const auto sent = client.sendPacket(alaw);
  CHECK_FALSE(sent.ok);
  CHECK(sent.error == "talk channel not open");
}

TEST_CASE("the live voice encoder keeps its filter state across packets")
{
  TapoVoiceEncoder whole;
  TapoVoiceEncoder split;
  std::vector<int16_t> voice(1920);
  for (size_t i = 0; i < voice.size(); ++i)
    voice[i] = static_cast<int16_t>(8000.0 * std::sin(static_cast<double>(i) * 0.3));
  const auto once = whole.encode(voice);
  auto first = split.encode(std::span(voice).first(960));
  const auto second = split.encode(std::span(voice).subspan(960));
  first.insert(first.end(), second.begin(), second.end());
  CHECK(once == first);
}
