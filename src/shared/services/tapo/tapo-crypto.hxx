#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tapo_crypto
{

struct AesInput
{
  std::vector<uint8_t> data;
  std::vector<uint8_t> key;
  std::vector<uint8_t> iv;
};

struct DigestInput
{
  std::string username;
  std::string password;
  std::string realm;
  std::string nonce;
  std::string qop;
  std::string opaque;
  std::string algorithm;
  std::string method;
  std::string uri;
  std::string cnonce;
  uint32_t nonceCount;
};

struct DigestChallenge
{
  std::string realm;
  std::string nonce;
  std::string qop;
  std::string opaque;
  std::string algorithm;
  std::string encryptType;
};

std::vector<uint8_t> md5Raw(const std::string& data);
std::vector<uint8_t> sha256Raw(const std::string& data);
std::vector<uint8_t> sha1Raw(const std::string& data);

std::string md5Hex(const std::string& data);
std::string sha256Hex(const std::string& data);
std::string sha1Hex(const std::string& data);

std::string toHex(const std::vector<uint8_t>& data, bool upper = true);
std::vector<uint8_t> fromHex(const std::string& hex);

std::string base64Encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base64Decode(const std::string& data);

std::string randomHex(size_t chars);
std::vector<uint8_t> randomBytes(size_t count);

std::vector<uint8_t> aes128CbcEncrypt(const AesInput& input);
std::vector<uint8_t> aes128CbcDecrypt(const AesInput& input);

DigestChallenge parseDigestChallenge(const std::string& header);
std::string buildDigestHeader(const DigestInput& input);

std::vector<uint8_t> toBytes(const std::string& text);
std::string toText(const std::vector<uint8_t>& data);

} // namespace tapo_crypto
