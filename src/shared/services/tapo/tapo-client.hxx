#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared/services/tapo/tapo-transport.hxx>
#include <string>
#include <vector>

enum class TapoTransportPreference : uint8_t
{
  Auto = 0,
  SecurePassthrough,
  LegacyStok
};

inline TapoTransportPreference tapoTransportPreferenceFromString(const std::string& value)
{
  if (value == "secure_passthrough")
    return TapoTransportPreference::SecurePassthrough;
  if (value == "legacy_stok")
    return TapoTransportPreference::LegacyStok;
  return TapoTransportPreference::Auto;
}

struct TapoCredentialCandidate
{
  std::string label;
  std::string username;
  std::string password;
};

struct TapoClientConfig
{
  std::string host;
  int port{443};
  std::vector<TapoCredentialCandidate> candidates;
  TapoTransportPreference transport{TapoTransportPreference::Auto};
  int connectTimeoutMs{3000};
  int requestTimeoutMs{5000};
  int loginAttempts{2};
};

class TapoClient
{
public:
  explicit TapoClient(TapoClientConfig config);

  TapoResult connect();
  TapoResult invoke(const Json::Value& request);
  TapoResult batch(const std::vector<Json::Value>& requests);

  bool isConnected() const;
  Json::Value state() const;
  const std::string& credentialLabel() const;

private:
  std::unique_ptr<ITapoTransport> makeTransport(TapoTransportKind kind,
                                                const TapoCredentialCandidate& candidate);
  TapoResult tryCandidate(const TapoCredentialCandidate& candidate);

  TapoClientConfig config_;
  mutable std::mutex mutex_;
  std::unique_ptr<ITapoTransport> transport_;
  std::string credentialLabel_;
};
