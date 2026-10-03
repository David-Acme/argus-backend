#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <feature/pairing/infra/pairing-banner.hxx>

#include <algorithm>
#include <string>

namespace
{
pairing_banner::PairingBannerInput sample()
{
  return {.code = "A1B2C3D4E5",
          .serverName = "Casa",
          .port = 7044,
          .instanceId = std::string(64, 'a'),
          .caFingerprint = std::string(64, 'b'),
          .serverFingerprint = std::string(64, 'c')};
}
}

TEST_CASE("the QR payload carries every key the app requires")
{
  const Json::Value json = pairing_banner::payload(sample());
  CHECK(json["host"].asString() == "casa.local");
  CHECK(json["port"].asInt() == 7044);
  CHECK(json["scheme"].asString() == "https");
  CHECK(json["code"].asString() == "A1B2C3D4E5");
  CHECK(json["instanceId"].asString().size() == 64);
  CHECK(json["caFingerprint"].asString().size() == 64);
  CHECK(json["serverFingerprint"].asString().size() == 64);
}

TEST_CASE("an unnamed server is argus.local")
{
  auto input = sample();
  input.serverName.clear();
  CHECK(pairing_banner::payload(input)["host"].asString() == "argus.local");
}

TEST_CASE("the banner shows the QR, the code and the address, and nothing without a code")
{
  const std::string banner = pairing_banner::render(sample());
  CHECK(banner.find("A1B2C3D4E5") != std::string::npos);
  CHECK(banner.find("https://casa.local:7044") != std::string::npos);
  CHECK(banner.find("█") != std::string::npos);
  auto empty = sample();
  empty.code.clear();
  CHECK(pairing_banner::render(empty).empty());
}

TEST_CASE("the ASCII QR is square: one line per two module rows")
{
  const std::string qr = pairing_banner::asciiQr("argus");
  const auto lines = static_cast<int>(std::count(qr.begin(), qr.end(), '\n'));
  CHECK(lines >= 15);
}
