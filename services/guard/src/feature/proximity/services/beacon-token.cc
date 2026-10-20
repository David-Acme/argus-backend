#include "beacon-token.hxx"

#include <drogon/utils/Utilities.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace
{
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr size_t kUuidHexLength = 32;

std::string hexOf(std::span<const uint8_t> bytes)
{
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const uint8_t byte : bytes) {
    hex.push_back(kHexDigits[byte >> 4U]);
    hex.push_back(kHexDigits[byte & 0x0FU]);
  }
  return hex;
}
}

std::string mintBeaconSeed()
{
  std::array<unsigned char, kBeaconSeedBytes> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    throw std::runtime_error("no random bytes for a beacon seed");
  return drogon::utils::base64Encode(bytes.data(), bytes.size());
}

std::optional<std::vector<uint8_t>> beaconSeedBytes(std::string_view base64)
{
  if (base64.empty() || base64.size() % 4 != 0)
    return std::nullopt;
  const std::string decoded = drogon::utils::base64Decode(base64);
  if (decoded.size() < kBeaconTokenBytes)
    return std::nullopt;
  return std::vector<uint8_t>(decoded.begin(), decoded.end());
}

int64_t beaconCounter(const BeaconClock& clock)
{
  const int64_t nowSeconds = clock.nowSeconds;
  const int64_t period = std::max<int64_t>(clock.rotateSeconds, 1);
  const int64_t quotient = nowSeconds / period;
  return (nowSeconds % period != 0 && nowSeconds < 0) ? quotient - 1 : quotient;
}

BeaconToken beaconToken(std::span<const uint8_t> seed, uint64_t counter)
{
  std::array<unsigned char, sizeof(uint64_t)> message{};
  for (size_t index = 0; index < message.size(); ++index)
    message.at(message.size() - 1 - index) = static_cast<unsigned char>((counter >> (8U * index)) & 0xFFU);
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  BeaconToken token{};
  if (HMAC(EVP_sha256(), seed.data(), static_cast<int>(seed.size()), message.data(), message.size(), digest.data(),
           &length) == nullptr ||
      length < kBeaconTokenBytes)
    return token;
  std::copy_n(digest.begin(), kBeaconTokenBytes, token.begin());
  return token;
}

std::string beaconUuid(const BeaconToken& token)
{
  const std::string hex = hexOf(token);
  return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
         hex.substr(20, 12);
}

std::optional<std::string> normalizedUuid(std::string_view text)
{
  std::string hex;
  hex.reserve(kUuidHexLength);
  for (const char raw : text) {
    if (raw == '-' || raw == ':')
      continue;
    const auto lowered = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
    if (kHexDigits.find(lowered) == std::string_view::npos)
      return std::nullopt;
    hex.push_back(lowered);
  }
  if (hex.size() != kUuidHexLength)
    return std::nullopt;
  return hex;
}

std::unordered_map<std::string, int64_t> beaconTable(const BeaconTableInput& input)
{
  std::unordered_map<std::string, int64_t> table;
  const int64_t current = beaconCounter({.nowSeconds = input.nowSeconds, .rotateSeconds = input.rotateSeconds});
  for (const BeaconSeed& seed : input.seeds) {
    if (seed.seed.size() < kBeaconTokenBytes)
      continue;
    for (int64_t counter = current - 1; counter <= current + 1; ++counter) {
      if (counter < 0)
        continue;
      BeaconToken token = beaconToken(seed.seed, static_cast<uint64_t>(counter));
      table.emplace(hexOf(token), seed.userId);
      std::ranges::reverse(token);
      table.emplace(hexOf(token), seed.userId);
    }
  }
  return table;
}
