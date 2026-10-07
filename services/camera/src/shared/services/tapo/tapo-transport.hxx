#pragma once

#include <cstdint>
#include <json/value.h>
#include <memory>
#include <shared/services/tapo/tapo-trust.hxx>
#include <string>

enum class TapoTransportKind : uint8_t
{
  SecurePassthrough = 0,
  LegacyStok
};

inline std::string tapoTransportKindToString(TapoTransportKind kind)
{
  switch (kind) {
    case TapoTransportKind::SecurePassthrough:
      return "secure_passthrough";
    case TapoTransportKind::LegacyStok:
      return "legacy_stok";
  }
  return "secure_passthrough";
}

inline TapoTransportKind tapoTransportKindFromString(const std::string& value)
{
  if (value == "legacy_stok")
    return TapoTransportKind::LegacyStok;
  return TapoTransportKind::SecurePassthrough;
}

enum class TapoHashAlgorithm : uint8_t
{
  Sha256 = 0,
  Md5
};

inline std::string tapoHashAlgorithmToString(TapoHashAlgorithm algorithm)
{
  return algorithm == TapoHashAlgorithm::Md5 ? "md5" : "sha256";
}

struct TapoCredentials
{
  std::string host;
  int port{443};
  std::string username;
  std::string password;
  int connectTimeoutMs{3000};
  int requestTimeoutMs{5000};
  std::shared_ptr<TapoTrust> trust;
};

enum class TapoFailureKind : uint8_t
{
  None = 0,
  Transport,
  NotSecure,
  CredentialRefused,
  LockedOut
};

inline std::string tapoFailureKindToString(TapoFailureKind kind)
{
  switch (kind) {
    case TapoFailureKind::None:
      return "none";
    case TapoFailureKind::Transport:
      return "transport";
    case TapoFailureKind::NotSecure:
      return "not_secure";
    case TapoFailureKind::CredentialRefused:
      return "credential_refused";
    case TapoFailureKind::LockedOut:
      return "locked_out";
  }
  return "none";
}

struct TapoResult
{
  bool ok{false};
  int errorCode{0};
  std::string error;
  Json::Value data;
  TapoFailureKind kind{TapoFailureKind::None};
  int secLeft{0};

  static TapoResult failure(const std::string& message, int code = 0)
  {
    TapoResult result;
    result.ok = false;
    result.error = message;
    result.errorCode = code;
    return result;
  }

  [[nodiscard]] TapoResult as(TapoFailureKind failureKind, int secondsLeft = 0) const
  {
    TapoResult copy = *this;
    copy.kind = failureKind;
    copy.secLeft = secondsLeft;
    return copy;
  }

  static TapoResult success(Json::Value payload)
  {
    TapoResult result;
    result.ok = true;
    result.data = std::move(payload);
    return result;
  }
};

struct TapoTransportRequest
{
  TapoTransportKind kind{TapoTransportKind::SecurePassthrough};
  TapoCredentials credentials;
};

class ITapoTransport
{
public:
  virtual ~ITapoTransport() = default;

  virtual TapoResult login() = 0;
  virtual TapoResult request(const Json::Value& payload) = 0;
  virtual TapoTransportKind kind() const = 0;
  virtual Json::Value state() const = 0;
  virtual bool isAuthenticated() const = 0;
};
