#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace s3_signing
{

struct SigV4Input
{
  std::string method;
  std::string canonicalUri;
  std::string canonicalQuery;
  std::map<std::string, std::string> headers;
  std::string payloadHash;
  std::string accessKey;
  std::string secretKey;
  std::string region;
  std::string timestamp;
};

struct SignedRequest
{
  std::string authorization;
  std::string signature;
};

inline std::string hexLower(const unsigned char* data, size_t size)
{
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    output.push_back(kHex[data[i] >> 4U]);
    output.push_back(kHex[data[i] & 0x0FU]);
  }
  return output;
}

inline std::string sha256Hex(std::string_view input)
{
  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int size = 0;
  if (EVP_Digest(input.data(), input.size(), digest, &size, EVP_sha256(),
                 nullptr) != 1) {
    throw std::runtime_error("Unable to hash S3 payload");
  }
  return hexLower(digest, size);
}

inline std::string hmacSha256(std::string_view key, std::string_view input)
{
  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int size = 0;
  if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char*>(input.data()), input.size(),
            digest, &size)) {
    throw std::runtime_error("Unable to sign S3 request");
  }
  return {reinterpret_cast<const char*>(digest), size};
}

inline std::string normalizeHeaderValue(std::string_view value)
{
  std::string output;
  output.reserve(value.size());
  bool spacing = false;
  for (const unsigned char c : value) {
    if (std::isspace(c)) {
      spacing = !output.empty();
      continue;
    }
    if (spacing) {
      output.push_back(' ');
      spacing = false;
    }
    output.push_back(static_cast<char>(c));
  }
  return output;
}

inline SignedRequest sign(const SigV4Input& input)
{
  if (input.timestamp.size() < 8 || input.method.empty() ||
      input.canonicalUri.empty() || input.payloadHash.empty() ||
      input.accessKey.empty() || input.secretKey.empty() || input.region.empty()) {
    throw std::invalid_argument("Incomplete S3 signing input");
  }

  std::string canonicalHeaders;
  std::string signedHeaders;
  for (const auto& [name, value] : input.headers) {
    if (!signedHeaders.empty())
      signedHeaders.push_back(';');
    signedHeaders += name;
    canonicalHeaders += name + ":" + normalizeHeaderValue(value) + "\n";
  }
  if (signedHeaders.empty())
    throw std::invalid_argument("S3 requests require signed headers");

  const std::string canonicalRequest =
      input.method + "\n" + input.canonicalUri + "\n" +
      input.canonicalQuery + "\n" + canonicalHeaders + "\n" +
      signedHeaders + "\n" + input.payloadHash;
  const std::string date = input.timestamp.substr(0, 8);
  const std::string scope = date + "/" + input.region + "/s3/aws4_request";
  const std::string stringToSign = "AWS4-HMAC-SHA256\n" + input.timestamp +
                                   "\n" + scope + "\n" +
                                   sha256Hex(canonicalRequest);

  const auto dateKey = hmacSha256("AWS4" + input.secretKey, date);
  const auto regionKey = hmacSha256(dateKey, input.region);
  const auto serviceKey = hmacSha256(regionKey, "s3");
  const auto signingKey = hmacSha256(serviceKey, "aws4_request");
  const auto signatureBytes = hmacSha256(signingKey, stringToSign);
  const auto signature = hexLower(
      reinterpret_cast<const unsigned char*>(signatureBytes.data()),
      signatureBytes.size());

  return {
      .authorization = "AWS4-HMAC-SHA256 Credential=" + input.accessKey + "/" +
                       scope + ",SignedHeaders=" + signedHeaders + ",Signature=" +
                       signature,
      .signature = signature,
  };
}

} // namespace s3_signing
