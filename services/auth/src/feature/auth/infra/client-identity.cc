#include "client-identity.hxx"

#include <string>

namespace
{
constexpr std::string_view kStablePrefix = "Argus/1 (";
constexpr std::string_view kStableSuffix = ")";
constexpr int kHexRadix = 16;

std::optional<int> hexValue(char digit)
{
  if (digit >= '0' && digit <= '9')
    return digit - '0';
  if (digit >= 'a' && digit <= 'f')
    return digit - 'a' + 10;
  if (digit >= 'A' && digit <= 'F')
    return digit - 'A' + 10;
  return std::nullopt;
}

std::optional<std::string> percentDecoded(std::string_view encoded)
{
  std::string decoded;
  decoded.reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const char current = encoded[index];
    if (current != '%') {
      decoded.push_back(current);
      continue;
    }
    if (index + 2 >= encoded.size())
      return std::nullopt;
    const auto high = hexValue(encoded[index + 1]);
    const auto low = hexValue(encoded[index + 2]);
    if (!high || !low)
      return std::nullopt;
    decoded.push_back(static_cast<char>((*high * kHexRadix) + *low));
    index += 2;
  }
  return decoded;
}

std::size_t utf8SequenceLength(unsigned char lead)
{
  if (lead < 0x80U)
    return 1;
  if ((lead & 0xE0U) == 0xC0U && lead >= 0xC2U)
    return 2;
  if ((lead & 0xF0U) == 0xE0U)
    return 3;
  if ((lead & 0xF8U) == 0xF0U && lead <= 0xF4U)
    return 4;
  return 0;
}

bool continuation(unsigned char byte)
{
  return (byte & 0xC0U) == 0x80U;
}

bool controlCharacter(std::string_view character)
{
  const auto lead = static_cast<unsigned char>(character.front());
  if (character.size() == 1)
    return lead < 0x20U || lead == 0x7FU;
  if (character.size() == 2 && lead == 0xC2U)
    return static_cast<unsigned char>(character[1]) < 0xA0U;
  return false;
}
}

std::optional<SessionPlatform>
client_identity::platformOfStableUserAgent(std::string_view userAgent)
{
  if (!userAgent.starts_with(kStablePrefix) ||
      !userAgent.ends_with(kStableSuffix))
    return std::nullopt;
  const std::string name(userAgent.substr(
      kStablePrefix.size(),
      userAgent.size() - kStablePrefix.size() - kStableSuffix.size()));
  const SessionPlatform platform = sessionPlatformFromString(name);
  if (platform == SessionPlatform::Unknown)
    return std::nullopt;
  return platform;
}

bool client_identity::isStableUserAgent(std::string_view userAgent)
{
  return platformOfStableUserAgent(userAgent).has_value();
}

SessionPlatform client_identity::platformOfClientHeader(std::string_view value)
{
  const auto slash = value.find('/');
  if (slash == std::string_view::npos || slash == 0)
    return SessionPlatform::Unknown;
  return sessionPlatformFromString(std::string(value.substr(0, slash)));
}

std::string client_identity::decodeDeviceName(std::string_view encoded)
{
  const auto decoded = percentDecoded(encoded);
  if (!decoded)
    return {};

  std::string name;
  name.reserve(decoded->size());
  std::size_t characters = 0;
  std::size_t index = 0;
  while (index < decoded->size() && characters < kMaxDeviceNameChars) {
    const std::size_t length =
        utf8SequenceLength(static_cast<unsigned char>((*decoded)[index]));
    if (length == 0 || index + length > decoded->size())
      return {};
    const std::string_view character =
        std::string_view(*decoded).substr(index, length);
    for (std::size_t offset = 1; offset < length; ++offset) {
      if (!continuation(static_cast<unsigned char>(character[offset])))
        return {};
    }
    index += length;
    if (controlCharacter(character))
      continue;
    name.append(character);
    ++characters;
  }

  const auto first = name.find_first_not_of(' ');
  if (first == std::string::npos)
    return {};
  const auto last = name.find_last_not_of(' ');
  return name.substr(first, last - first + 1);
}

ClientIdentity client_identity::of(const drogon::HttpRequestPtr& req)
{
  ClientIdentity identity;
  identity.platform =
      platformOfClientHeader(req->getHeader(std::string(kClientHeader)));
  if (identity.platform == SessionPlatform::Unknown) {
    if (const auto platform =
            platformOfStableUserAgent(req->getHeader("User-Agent")))
      identity.platform = *platform;
  }
  identity.deviceName =
      decodeDeviceName(req->getHeader(std::string(kDeviceHeader)));
  return identity;
}
