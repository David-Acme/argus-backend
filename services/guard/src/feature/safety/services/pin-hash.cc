#include "pin-hash.hxx"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <vector>

namespace
{
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;
constexpr int kMinIterations = 100000;
constexpr size_t kMinPinDigits = 4;
constexpr size_t kMaxPinDigits = 8;

std::string toHex(const unsigned char* bytes, size_t size)
{
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string hex;
  hex.reserve(size * 2);
  for (size_t index = 0; index < size; ++index) {
    hex.push_back(kDigits[bytes[index] >> 4U]);
    hex.push_back(kDigits[bytes[index] & 0x0FU]);
  }
  return hex;
}

std::vector<unsigned char> fromHex(std::string_view hex)
{
  if (hex.size() % 2 != 0)
    return {};
  std::vector<unsigned char> bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t index = 0; index < hex.size(); index += 2) {
    unsigned value = 0;
    const auto [end, error] =
        std::from_chars(hex.data() + index, hex.data() + index + 2, value, 16);
    if (error != std::errc{} || end != hex.data() + index + 2)
      return {};
    bytes.push_back(static_cast<unsigned char>(value));
  }
  return bytes;
}

struct DeriveInput
{
  std::string_view pin;
  const std::vector<unsigned char>& salt;
  int iterations{0};
};

std::array<unsigned char, kHashBytes> derive(const DeriveInput& input)
{
  std::array<unsigned char, kHashBytes> out{};
  if (PKCS5_PBKDF2_HMAC(input.pin.data(), static_cast<int>(input.pin.size()),
                        input.salt.data(), static_cast<int>(input.salt.size()),
                        input.iterations, EVP_sha256(), static_cast<int>(out.size()),
                        out.data()) != 1)
    throw std::runtime_error("pin hash derivation failed");
  return out;
}

std::vector<std::string_view> split(std::string_view text)
{
  std::vector<std::string_view> parts;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('$', start);
    if (end == std::string_view::npos) {
      parts.push_back(text.substr(start));
      break;
    }
    parts.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}
}

bool pin_hash::wellFormedPin(std::string_view pin)
{
  return pin.size() >= kMinPinDigits && pin.size() <= kMaxPinDigits &&
         std::ranges::all_of(pin, [](char c) { return c >= '0' && c <= '9'; });
}

bool pin_hash::trivialPin(std::string_view pin)
{
  if (pin.size() < 2)
    return true;
  const auto allSame = std::ranges::all_of(pin, [first = pin.front()](char c) { return c == first; });
  bool ascending = true;
  bool descending = true;
  for (size_t index = 1; index < pin.size(); ++index) {
    const int step = pin[index] - pin[index - 1];
    ascending = ascending && (step == 1 || (pin[index - 1] == '9' && pin[index] == '0'));
    descending = descending && (step == -1 || (pin[index - 1] == '0' && pin[index] == '9'));
  }
  const size_t half = pin.size() / 2;
  const bool repeatedHalves =
      pin.size() % 2 == 0 && half >= 2 && pin.substr(0, half) == pin.substr(half);
  return allSame || ascending || descending || repeatedHalves;
}

std::string pin_hash::make(const HashInput& input)
{
  std::vector<unsigned char> salt(kSaltBytes);
  if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
    throw std::runtime_error("pin salt generation failed");
  const int iterations = std::max(input.iterations, kMinIterations);
  const auto hash = derive({.pin = input.pin, .salt = salt, .iterations = iterations});
  return std::string(kScheme) + "$" + std::to_string(iterations) + "$" +
         toHex(salt.data(), salt.size()) + "$" + toHex(hash.data(), hash.size());
}

bool pin_hash::verify(const VerifyInput& input)
{
  const auto parts = split(input.stored);
  if (parts.size() != 4 || parts[0] != kScheme)
    return false;
  int iterations = 0;
  const auto [end, error] =
      std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), iterations);
  if (error != std::errc{} || end != parts[1].data() + parts[1].size() ||
      iterations < kMinIterations)
    return false;
  const auto salt = fromHex(parts[2]);
  const auto expected = fromHex(parts[3]);
  if (salt.empty() || expected.size() != kHashBytes)
    return false;
  const auto actual = derive({.pin = input.pin, .salt = salt, .iterations = iterations});
  return CRYPTO_memcmp(actual.data(), expected.data(), kHashBytes) == 0;
}
