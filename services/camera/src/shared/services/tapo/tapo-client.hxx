#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared/services/tapo/tapo-control-status.hxx>
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
  std::string memoryKey;
};

struct TapoWinnerMemory
{
  std::string label;
  std::string key;
};

using TapoTransportFactory =
    std::function<std::unique_ptr<ITapoTransport>(const TapoTransportRequest&)>;

struct TapoClientConfig
{
  std::string host;
  int port{443};
  std::vector<TapoCredentialCandidate> candidates;
  TapoTransportPreference transport{TapoTransportPreference::Auto};
  int connectTimeoutMs{3000};
  int requestTimeoutMs{5000};
  int loginAttempts{2};
  std::shared_ptr<TapoTrust> trust;
  TapoWinnerMemory remembered;
  std::function<void(const TapoWinnerMemory&)> persistWinner;
  TapoTransportFactory transportFactory;
  std::function<int64_t()> nowMs;
  int lockoutMarginSeconds{2};
  int refusedRetrySeconds{600};
  int unreachableBackoffMinSeconds{5};
  int unreachableBackoffMaxSeconds{60};
};

class TapoClient
{
public:
  explicit TapoClient(TapoClientConfig config);

  TapoResult connect();
  TapoResult ensureConnected();
  TapoResult invoke(const Json::Value& request);
  TapoResult batch(const std::vector<Json::Value>& requests);

  bool isConnected() const;
  Json::Value state() const;
  std::string credentialLabel() const;
  TapoControlStatus status() const;
  std::string endpoint() const;

private:
  enum class Verdict : uint8_t
  {
    Unknown = 0,
    Won,
    Refused
  };

  struct Standing
  {
    Verdict verdict{Verdict::Unknown};
    int refusals{0};
  };

  TapoResult connectLocked(std::optional<TapoTransportKind> excludedKind);
  TapoResult attempt(std::size_t index, std::optional<TapoTransportKind> excludedKind);
  std::vector<std::size_t> order() const;
  void adopt(std::size_t index);
  void refuse(std::size_t index);
  void enterRefused();
  void enterLocked(const TapoResult& result);
  void enterUnreachable(const TapoResult& result);
  TapoResult blockedFailure() const;
  void publish(TapoControlState state, const TapoResult& cause);
  std::string refusedLabels() const;
  static bool isRecoverableFailure(const TapoResult& result);
  static bool isSafeToRetry(const Json::Value& request);

  TapoClientConfig config_;
  mutable std::mutex mutex_;
  std::unique_ptr<ITapoTransport> transport_;
  std::string credentialLabel_;
  std::vector<Standing> standing_;
  std::optional<std::size_t> winner_;
  std::size_t retryCursor_{0};
  std::string persistedLabel_;
  int64_t blockedUntilMs_{0};
  int unreachableBackoffSeconds_{0};
  TapoControlState state_{TapoControlState::Idle};
  int blockedCode_{0};
  mutable std::mutex statusMutex_;
  TapoControlStatus published_;
};
