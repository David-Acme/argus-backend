#pragma once

#include <cstdint>
#include <grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace argus::sdk
{

// Secret comparison that does not return early on the first differing byte.
inline bool constantTimeEquals(const std::string& left,
                               const std::string& right)
{
  if (left.size() != right.size())
    return false;
  unsigned char diff = 0;
  for (std::string::size_type index = 0; index < left.size(); ++index)
    diff |= static_cast<unsigned char>(left[index]) ^
            static_cast<unsigned char>(right[index]);
  return diff == 0;
}

inline std::string metadata(const grpc::CallbackServerContext* context,
                            std::string_view key)
{
  for (const auto& [metadataKey, value] : context->client_metadata()) {
    if (std::string_view(metadataKey.data(), metadataKey.size()) == key)
      return std::string(value.data(), value.size());
  }
  return {};
}

// One directed capability: the credential is known only by the caller and the
// receiver of that single edge, and the service name is receiver-side config.
struct CallerCredential
{
  std::string service;
  std::string secret;
};

inline std::vector<CallerCredential> callerCredentialsFromPairs(
    const std::vector<std::pair<std::string, std::string>>& pairs)
{
  std::vector<CallerCredential> credentials;
  credentials.reserve(pairs.size());
  for (const auto& [service, secret] : pairs) {
    if (service.empty() || secret.empty())
      continue;
    credentials.push_back({.service = service, .secret = secret});
  }
  return credentials;
}

// Caller authority comes from the credential that matched, never from declared
// metadata; the receiver chooses the credential set per RPC.
inline std::optional<std::string>
authorizeCaller(const grpc::CallbackServerContext* context,
                std::span<const CallerCredential> credentials)
{
  const std::string presented = metadata(context, kCallerCredentialKey);
  if (presented.empty())
    return std::nullopt;
  for (const auto& credential : credentials) {
    if (!credential.secret.empty() &&
        constantTimeEquals(presented, credential.secret))
      return credential.service;
  }
  return std::nullopt;
}

// Reads the x-argus-user / x-argus-role / x-argus-device metadata the client
// base attaches; nullopt when any leg is missing or the user id is invalid.
inline std::optional<int64_t>
callerUserId(const grpc::CallbackServerContext* context)
{
  bool user = false;
  bool role = false;
  bool device = false;
  int64_t id = 0;
  for (const auto& [key, value] : context->client_metadata()) {
    if (key == "x-argus-user") {
      user = true;
      try {
        id = std::stoll(std::string(value.data(), value.size()));
      }
      catch (const std::exception&) {
        return std::nullopt;
      }
    }
    else if (key == "x-argus-role")
      role = true;
    else if (key == "x-argus-device")
      device = true;
  }
  if (!(user && role && device))
    return std::nullopt;
  return id;
}

} // namespace argus::sdk
