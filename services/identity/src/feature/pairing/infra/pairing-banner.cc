#include "pairing-banner.hxx"

#include <cert/cert-service.hxx>
#include <feature/pairing/infra/pairing-code.hxx>
#include <config/config-service.hxx>
#include <text/json-util.hxx>

#include <qrcodegen/qrcodegen.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

namespace
{
constexpr int kQuietZone = 4;
constexpr std::string_view kScheme = "https";
constexpr std::string_view kHostSuffix = ".local";
constexpr std::string_view kDefaultServerName = "Argus";

std::string hostOf(std::string_view serverName)
{
  std::string host(serverName.empty() ? kDefaultServerName : serverName);
  std::ranges::transform(host, host.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  host += kHostSuffix;
  return host;
}
}

namespace pairing_banner
{
Json::Value payload(const PairingBannerInput& input)
{
  Json::Value json(Json::objectValue);
  json["host"] = hostOf(input.serverName);
  json["port"] = input.port;
  json["scheme"] = std::string(kScheme);
  json["code"] = input.code;
  json["instanceId"] = input.instanceId;
  json["caFingerprint"] = input.caFingerprint;
  json["serverFingerprint"] = input.serverFingerprint;
  return json;
}

std::string asciiQr(std::string_view text)
{
  const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
      std::string(text).c_str(), qrcodegen::QrCode::Ecc::LOW);
  const int span = qr.getSize() + 2 * kQuietZone;
  const auto dark = [&qr](int x, int y) {
    return qr.getModule(x - kQuietZone, y - kQuietZone);
  };
  std::ostringstream out;
  for (int y = 0; y < span; y += 2) {
    for (int x = 0; x < span; ++x) {
      const bool top = dark(x, y);
      const bool bottom = y + 1 < span && dark(x, y + 1);
      if (top && bottom)
        out << "█";
      else if (top)
        out << "▀";
      else if (bottom)
        out << "▄";
      else
        out << ' ';
    }
    out << '\n';
  }
  return out.str();
}

std::string render(const PairingBannerInput& input)
{
  if (input.code.empty())
    return {};
  const Json::Value json = payload(input);
  std::ostringstream out;
  out << "\n"
      << "  ARGUS: pairing\n\n"
      << asciiQr(json_util::toString(json)) << "\n"
      << "  Scan this code with the Argus app, or type the pairing code:\n\n"
      << "    " << input.code << "\n\n"
      << "  Server: " << kScheme << "://" << json["host"].asString() << ":"
      << input.port << "\n\n";
  return out.str();
}

void printWhenUnpaired(int port)
{
  if (ConfigService::getBool("pairing.paired"))
    return;
  const std::string banner =
      render({.code = PairingCodeStore(PairingCodeStore::defaultPath()).current().value_or(""),
              .serverName = ConfigService::getString("mdns.name"),
              .port = port,
              .instanceId = CertService::instanceId(),
              .caFingerprint = CertService::caFingerprint(),
              .serverFingerprint = CertService::serverFingerprint()});
  if (!banner.empty())
    std::cout << banner << std::flush;
}
}
