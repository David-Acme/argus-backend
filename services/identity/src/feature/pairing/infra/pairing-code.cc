#include "pairing-code.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <config/config-service.hxx>
#include <filesystem>
#include <fstream>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <system_error>
#include <utility>
#include <trantor/utils/Logger.h>

namespace
{
constexpr std::string_view kBase32Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
constexpr std::size_t kMinLegacyLength = 8;
constexpr std::size_t kMaxLegacyLength = 12;
constexpr std::string_view kCodeFile = "pairing.code";
constexpr std::string_view kDefaultCertDir = "certs";

bool isBase32Symbol(char symbol)
{
  return kBase32Alphabet.find(symbol) != std::string_view::npos;
}

bool isHexSymbol(char symbol)
{
  return (symbol >= '0' && symbol <= '9') || (symbol >= 'A' && symbol <= 'F');
}
}

std::string pairing_code::normalize(std::string_view raw)
{
  std::string code;
  code.reserve(raw.size());
  for (const char symbol : raw) {
    if (symbol == '-' || std::isspace(static_cast<unsigned char>(symbol)) != 0)
      continue;
    code.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(symbol))));
  }
  return code;
}

bool pairing_code::isLegacy(std::string_view code)
{
  return code.size() >= kMinLegacyLength && code.size() <= kMaxLegacyLength &&
         std::ranges::all_of(code, isHexSymbol);
}

bool pairing_code::wellFormed(std::string_view code)
{
  if (isLegacy(code))
    return true;
  return code.size() >= kBase32Length && code.size() <= kMaxLength &&
         std::ranges::all_of(code, isBase32Symbol);
}

std::string pairing_code::encodeBase32(std::span<const unsigned char> bytes)
{
  std::string encoded;
  encoded.reserve((bytes.size() * 8 + 4) / 5);
  unsigned buffer = 0;
  int bits = 0;
  for (const unsigned char byte : bytes) {
    buffer = (buffer << 8U) | byte;
    bits += 8;
    while (bits >= 5) {
      encoded.push_back(kBase32Alphabet[(buffer >> static_cast<unsigned>(bits - 5)) & 0x1FU]);
      bits -= 5;
    }
  }
  if (bits > 0)
    encoded.push_back(kBase32Alphabet[(buffer << static_cast<unsigned>(5 - bits)) & 0x1FU]);
  return encoded;
}

std::optional<std::string> pairing_code::mint()
{
  std::array<unsigned char, kSecretBytes> secret{};
  if (RAND_bytes(secret.data(), static_cast<int>(secret.size())) != 1)
    return std::nullopt;
  auto code = encodeBase32(secret);
  OPENSSL_cleanse(secret.data(), secret.size());
  return code;
}

bool pairing_code::sameCode(std::string_view expected, std::string_view candidate)
{
  return !expected.empty() && expected.size() == candidate.size() &&
         CRYPTO_memcmp(expected.data(), candidate.data(), expected.size()) == 0;
}

std::string pairing_code::hmacHex(const PairingHmacInput& input)
{
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (HMAC(EVP_sha256(), input.code.data(), static_cast<int>(input.code.size()),
           reinterpret_cast<const unsigned char*>(input.message.data()),
           input.message.size(), digest.data(), &length) == nullptr)
    return {};
  constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string hex;
  hex.reserve(static_cast<std::size_t>(length) * 2);
  for (unsigned int i = 0; i < length; ++i) {
    hex.push_back(kHex[digest[i] >> 4]);
    hex.push_back(kHex[digest[i] & 0x0F]);
  }
  return hex;
}

PairingCodeStore::PairingCodeStore(std::string path) : path_(std::move(path)) {}

std::string PairingCodeStore::defaultPath()
{
  std::string dir = ConfigService::getString("cert.dir");
  if (dir.empty())
    dir = kDefaultCertDir;
  return (std::filesystem::path(dir) / kCodeFile).string();
}

std::optional<std::string> PairingCodeStore::current() const
{
  std::ifstream in(path_);
  std::string raw;
  if (!(in >> raw))
    return std::nullopt;
  auto code = pairing_code::normalize(raw);
  if (!pairing_code::wellFormed(code))
    return std::nullopt;
  return code;
}

bool PairingCodeStore::verifyCode(std::string_view candidate) const
{
  const auto code = current();
  return code && pairing_code::sameCode(*code, pairing_code::normalize(candidate));
}

bool PairingCodeStore::verifyProof(const PairingProofCheck& check) const
{
  const auto code = current();
  if (!code || check.nonce.empty())
    return false;
  const std::string message = "argus-pair-client|" + std::string(check.nonce);
  const auto expected = pairing_code::hmacHex({.code = *code, .message = message});
  return pairing_code::sameCode(expected, pairing_code::normalize(check.proof));
}

std::string PairingCodeStore::serverProof(const PairingServerProofInput& input) const
{
  const auto code = current();
  if (!code)
    return {};
  const std::string message = "argus-pair-server|" + std::string(input.nonce) + "|" +
                              std::string(input.caFingerprint);
  return pairing_code::hmacHex({.code = *code, .message = message});
}

bool PairingCodeStore::rotate() const
{
  const auto code = pairing_code::mint();
  if (!code)
    return false;
  const std::filesystem::path target(path_);
  const std::filesystem::path staged = target.string() + ".next";
  {
    std::ofstream out(staged, std::ios::trunc);
    if (!out)
      return false;
    out << *code << '\n';
    if (!out.flush())
      return false;
  }
  std::error_code error;
  std::filesystem::permissions(staged,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, error);
  std::filesystem::rename(staged, target, error);
  if (error) {
    LOG_WARN << "Pairing: the pairing code could not be rotated";
    std::filesystem::remove(staged, error);
    return false;
  }
  return true;
}
