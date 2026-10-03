#include "opaque-token.hxx"

#include <array>
#include <cstdint>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace
{
constexpr size_t kTokenBytes = 32;
constexpr size_t kTokenChars = 43;
constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::string_view kHex = "0123456789abcdef";
}

namespace opaque_token
{

std::optional<std::string> mint()
{
  std::array<unsigned char, kTokenBytes> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    return std::nullopt;

  std::string token;
  token.reserve(kTokenChars);
  uint32_t accumulator = 0;
  unsigned bits = 0;
  for (const unsigned char byte : bytes) {
    accumulator = (accumulator << 8U) | byte;
    bits += 8U;
    while (bits >= 6U) {
      bits -= 6U;
      token.push_back(kAlphabet[(accumulator >> bits) & 0x3FU]);
    }
  }
  if (bits > 0U)
    token.push_back(kAlphabet[(accumulator << (6U - bits)) & 0x3FU]);
  return token;
}

std::optional<std::string> sha256Hex(std::string_view token)
{
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_Digest(token.data(), token.size(), digest.data(), &length,
                 EVP_sha256(), nullptr) != 1)
    return std::nullopt;

  std::string output;
  output.reserve(static_cast<size_t>(length) * 2);
  for (unsigned int index = 0; index < length; ++index) {
    output.push_back(kHex[digest[index] >> 4U]);
    output.push_back(kHex[digest[index] & 0x0FU]);
  }
  return output;
}

}
