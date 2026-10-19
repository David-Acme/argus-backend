#include "turn-credentials.hxx"

#include <drogon/utils/Utilities.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace
{
constexpr std::size_t kMinSecretBytes = 32;

bool isHostCharacter(unsigned char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
}
}

bool turn_credentials::enabled(const SyncTurnConfig& relay)
{
  return !relay.host.empty() && relay.port != 0 && relay.secret.size() >= kMinSecretBytes &&
         relay.secret.find("CHANGE_ME") == std::string::npos &&
         std::ranges::all_of(relay.host,
                             [](char c) { return isHostCharacter(static_cast<unsigned char>(c)); });
}

TurnCredential turn_credentials::make(const TurnCredentialInput& input)
{
  TurnCredential out;
  out.expiresAt = input.expiresAt;
  out.username = std::to_string(input.expiresAt);
  out.username += ':';
  out.username += input.subject;
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digestLength = 0;
  const unsigned char* mac =
      HMAC(EVP_sha1(), input.secret.data(), static_cast<int>(input.secret.size()),
           reinterpret_cast<const unsigned char*>(out.username.data()), out.username.size(),
           digest.data(), &digestLength);
  if (mac == nullptr)
    return {};
  out.credential = drogon::utils::base64Encode(digest.data(), digestLength);
  return out;
}

std::vector<std::string> turn_credentials::urls(const SyncTurnConfig& relay)
{
  const std::string authority = relay.host + ":" + std::to_string(relay.port);
  std::vector<std::string> out{"turn:" + authority + "?transport=udp",
                               "turn:" + authority + "?transport=tcp"};
  if (relay.tlsPort != 0)
    out.push_back("turns:" + relay.host + ":" + std::to_string(relay.tlsPort) +
                  "?transport=tcp");
  return out;
}

IceServers turn_credentials::iceServers(const IceServersInput& input)
{
  IceServers out;
  if (!enabled(input.relay))
    return out;
  const TurnCredential credential =
      make({.secret = input.relay.secret,
            .subject = input.subject,
            .expiresAt = input.now + input.relay.ttl.count()});
  if (credential.credential.empty())
    return out;
  Json::Value list(Json::arrayValue);
  for (const auto& url : urls(input.relay))
    list.append(url);
  Json::Value server(Json::objectValue);
  server["urls"] = std::move(list);
  server["username"] = credential.username;
  server["credential"] = credential.credential;
  out.servers.append(std::move(server));
  out.expiresAt = credential.expiresAt;
  return out;
}
