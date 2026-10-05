#include "outbox-key.hxx"

#include <openssl/rand.h>
#include <text/sha256.hxx>

namespace outbox
{

std::string transitionId(const TransitionKey& key)
{
  std::string material(key.table);
  material += '|';
  material += std::to_string(key.recordId);
  material += '|';
  material.append(key.discriminator);
  std::string id(key.prefix);
  id += argus::hash::sha256Hex(material).substr(0, 32);
  return id;
}

std::string uniqueId(std::string_view prefix, const MsgIdEntropy& entropy)
{
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string id(prefix);
  id.reserve(id.size() + entropy.size() * 2);
  for (const auto byte : entropy) {
    id.push_back(kHex[byte >> 4U]);
    id.push_back(kHex[byte & 0x0FU]);
  }
  return id;
}

std::optional<MsgIdEntropy> drawEntropy()
{
  MsgIdEntropy bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    return std::nullopt;
  return bytes;
}

std::string legacyId(std::string_view prefix, int64_t rowId)
{
  std::string id(prefix);
  id += std::to_string(rowId);
  return id;
}

std::string fingerprint(std::string_view payload)
{
  return argus::hash::sha256Hex(payload);
}

}
