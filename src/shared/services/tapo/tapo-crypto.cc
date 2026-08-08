#include "tapo-crypto.hxx"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace
{

using CipherPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using DigestPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

std::vector<uint8_t> digest(const EVP_MD* algorithm, const std::string& data)
{
  DigestPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx)
    return {};
  unsigned int length = 0;
  std::vector<uint8_t> out(static_cast<size_t>(EVP_MAX_MD_SIZE));
  if (EVP_DigestInit_ex(ctx.get(), algorithm, nullptr) != 1)
    return {};
  if (EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1)
    return {};
  if (EVP_DigestFinal_ex(ctx.get(), out.data(), &length) != 1)
    return {};
  out.resize(length);
  return out;
}

std::string trim(const std::string& value)
{
  const auto begin = value.find_first_not_of(" \t\r\n\"");
  if (begin == std::string::npos)
    return {};
  const auto end = value.find_last_not_of(" \t\r\n\"");
  return value.substr(begin, end - begin + 1);
}

} // namespace

namespace tapo_crypto
{

std::vector<uint8_t> md5Raw(const std::string& data)
{
  return digest(EVP_md5(), data);
}

std::vector<uint8_t> sha256Raw(const std::string& data)
{
  return digest(EVP_sha256(), data);
}

std::vector<uint8_t> sha1Raw(const std::string& data)
{
  return digest(EVP_sha1(), data);
}

std::string md5Hex(const std::string& data)
{
  return toHex(md5Raw(data));
}

std::string sha256Hex(const std::string& data)
{
  return toHex(sha256Raw(data));
}

std::string sha1Hex(const std::string& data)
{
  return toHex(sha1Raw(data));
}

std::string toHex(const std::vector<uint8_t>& data, bool upper)
{
  static const char* kUpper = "0123456789ABCDEF";
  static const char* kLower = "0123456789abcdef";
  const char* table = upper ? kUpper : kLower;
  std::string out;
  out.reserve(data.size() * 2);
  for (const uint8_t byte : data) {
    out.push_back(table[byte >> 4]);
    out.push_back(table[byte & 0x0F]);
  }
  return out;
}

std::vector<uint8_t> fromHex(const std::string& hex)
{
  const auto value = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int high = value(hex[i]);
    const int low = value(hex[i + 1]);
    if (high < 0 || low < 0)
      return {};
    out.push_back(static_cast<uint8_t>((high << 4) | low));
  }
  return out;
}

std::string base64Encode(const std::vector<uint8_t>& data)
{
  if (data.empty())
    return {};
  std::string out(((data.size() + 2) / 3) * 4 + 1, '\0');
  const int written =
      EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data.data(),
                      static_cast<int>(data.size()));
  if (written < 0)
    return {};
  out.resize(static_cast<size_t>(written));
  return out;
}

std::vector<uint8_t> base64Decode(const std::string& data)
{
  if (data.empty())
    return {};
  std::vector<uint8_t> out((data.size() / 4) * 3 + 3);
  const int written =
      EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(data.data()),
                      static_cast<int>(data.size()));
  if (written < 0)
    return {};
  size_t size = static_cast<size_t>(written);
  for (auto it = data.rbegin(); it != data.rend() && *it == '='; ++it) {
    if (size == 0)
      break;
    --size;
  }
  out.resize(size);
  return out;
}

std::vector<uint8_t> randomBytes(size_t count)
{
  std::vector<uint8_t> out(count);
  if (RAND_bytes(out.data(), static_cast<int>(count)) != 1)
    return {};
  return out;
}

std::string randomHex(size_t chars)
{
  const auto bytes = randomBytes((chars + 1) / 2);
  auto hex = toHex(bytes);
  hex.resize(chars);
  return hex;
}

std::vector<uint8_t> aes128CbcEncrypt(const AesInput& input)
{
  if (input.key.size() != 16 || input.iv.size() != 16)
    return {};
  CipherPtr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!ctx)
    return {};
  if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, input.key.data(),
                         input.iv.data()) != 1)
    return {};
  std::vector<uint8_t> out(input.data.size() + 16);
  int length = 0;
  int total = 0;
  if (EVP_EncryptUpdate(ctx.get(), out.data(), &length, input.data.data(),
                        static_cast<int>(input.data.size())) != 1)
    return {};
  total = length;
  if (EVP_EncryptFinal_ex(ctx.get(), out.data() + total, &length) != 1)
    return {};
  total += length;
  out.resize(static_cast<size_t>(total));
  return out;
}

std::vector<uint8_t> aes128CbcDecrypt(const AesInput& input)
{
  if (input.key.size() != 16 || input.iv.size() != 16 || input.data.empty())
    return {};
  CipherPtr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!ctx)
    return {};
  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, input.key.data(),
                         input.iv.data()) != 1)
    return {};
  std::vector<uint8_t> out(input.data.size() + 16);
  int length = 0;
  int total = 0;
  if (EVP_DecryptUpdate(ctx.get(), out.data(), &length, input.data.data(),
                        static_cast<int>(input.data.size())) != 1)
    return {};
  total = length;
  if (EVP_DecryptFinal_ex(ctx.get(), out.data() + total, &length) != 1)
    return {};
  total += length;
  out.resize(static_cast<size_t>(total));
  return out;
}

DigestChallenge parseDigestChallenge(const std::string& header)
{
  DigestChallenge challenge;
  const auto extract = [&header](const std::string& key) -> std::string {
    const std::string needle = key + "=";
    size_t position = header.find(needle);
    while (position != std::string::npos) {
      const bool boundary =
          position == 0 || header[position - 1] == ' ' || header[position - 1] == ',';
      if (boundary)
        break;
      position = header.find(needle, position + 1);
    }
    if (position == std::string::npos)
      return {};
    position += needle.size();
    const bool quoted = position < header.size() && header[position] == '"';
    const size_t end = quoted ? header.find('"', position + 1) : header.find(',', position);
    if (end == std::string::npos)
      return trim(header.substr(position));
    return trim(header.substr(position, end - position + (quoted ? 1 : 0)));
  };
  challenge.realm = extract("realm");
  challenge.nonce = extract("nonce");
  challenge.qop = extract("qop");
  challenge.opaque = extract("opaque");
  challenge.algorithm = extract("algorithm");
  challenge.encryptType = extract("encrypt_type");
  return challenge;
}

std::string buildDigestHeader(const DigestInput& input)
{
  std::string algorithm = input.algorithm;
  std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  const bool useSha256 = algorithm.find("SHA-256") != std::string::npos ||
                         algorithm.find("SHA256") != std::string::npos;
  const auto hash = [useSha256](const std::string& value) {
    return useSha256 ? sha256Hex(value) : md5Hex(value);
  };

  const std::string ha1 =
      hash(input.username + ":" + input.realm + ":" + input.password);
  const std::string ha2 = hash(input.method + ":" + input.uri);

  char counter[9];
  std::snprintf(counter, sizeof(counter), "%08x", input.nonceCount);

  std::string response;
  if (input.qop.empty())
    response = hash(ha1 + ":" + input.nonce + ":" + ha2);
  else
    response = hash(ha1 + ":" + input.nonce + ":" + counter + ":" + input.cnonce +
                    ":" + input.qop + ":" + ha2);

  std::string header = "Digest username=\"" + input.username + "\", realm=\"" +
                       input.realm + "\", nonce=\"" + input.nonce + "\", uri=\"" +
                       input.uri + "\", response=\"" + response + "\"";
  if (!input.algorithm.empty())
    header += ", algorithm=" + input.algorithm;
  if (!input.qop.empty())
    header += ", qop=" + input.qop + ", nc=" + counter + ", cnonce=\"" +
              input.cnonce + "\"";
  if (!input.opaque.empty())
    header += ", opaque=\"" + input.opaque + "\"";
  return header;
}

std::vector<uint8_t> toBytes(const std::string& text)
{
  return std::vector<uint8_t>(text.begin(), text.end());
}

std::string toText(const std::vector<uint8_t>& data)
{
  return std::string(data.begin(), data.end());
}

} // namespace tapo_crypto
