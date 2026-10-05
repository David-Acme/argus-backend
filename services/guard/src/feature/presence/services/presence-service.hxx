#pragma once

#include <config/guard-config.hxx>
#include <feature/presence/services/presence-engine.hxx>
#include <feature/presence/services/presence-ports.hxx>
#include <shared/repositories/presence/presence-repository.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class NatsBus;

struct PresenceIngestInput
{
  int64_t userId{0};
  int64_t environmentId{0};
  PresenceSignalKind kind{PresenceSignalKind::LanSession};
  int64_t at{0};
};

struct PresenceUserView
{
  int64_t userId{0};
  PresenceState overall{PresenceState::Unknown};
  int64_t since{0};
  std::vector<PresenceRow> environments;
};

class PresenceService
{
public:
  struct Dependencies
  {
    NatsBus* bus{nullptr};
    const PresenceDirectory* directory{nullptr};
    PresencePublisher* publisher{nullptr};
    std::function<drogon::Task<void>(int64_t)> onAccountDisabled{};
  };

  static constexpr int64_t kConsentCacheSeconds = 300;
  static constexpr size_t kConsentCacheLimit = 4096;

  using Handler = drogon::Task<void> (*)(PresenceService&, Json::Value&&);

  PresenceService(Dependencies dependencies, GuardPresenceConfig config);
  ~PresenceService();

  PresenceService(const PresenceService&) = delete;
  PresenceService& operator=(const PresenceService&) = delete;

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;
  void refresh(const GuardPresenceConfig& config);

  [[nodiscard]] drogon::Task<void> ingest(PresenceIngestInput input);
  [[nodiscard]] drogon::Task<void> onSessionSignal(Json::Value event);
  [[nodiscard]] drogon::Task<void> onKnownSeen(Json::Value event);
  [[nodiscard]] drogon::Task<void> onIdentityChange(Json::Value event);
  [[nodiscard]] drogon::Task<void> sweep(int64_t now);
  [[nodiscard]] drogon::Task<void> reconcileConsent();

  [[nodiscard]] drogon::Task<std::vector<PresenceUserView>>
  snapshot(std::vector<int64_t> userIds) const;

private:
  [[nodiscard]] PresenceRules rules() const;
  [[nodiscard]] drogon::Task<bool> consented(int64_t userId);
  [[nodiscard]] drogon::Task<void> forget(int64_t userId);
  [[nodiscard]] drogon::Task<void> announce(PresenceRow row);
  void remember(int64_t userId, bool consent);
  [[nodiscard]] bool withdrawn(int64_t userId) const;
  void dispatch(std::string payload, Handler handler);
  void subscribe();
  void unsubscribe();

  struct ConsentEntry
  {
    bool consent{false};
    int64_t expiresAt{0};
  };

  Dependencies dependencies_;
  PresenceRepository repository_;
  mutable std::mutex mutex_;
  GuardPresenceConfig config_;
  std::unordered_map<int64_t, ConsentEntry> consent_;
  std::vector<uint64_t> subscriptions_;
  std::vector<uint64_t> timers_;
  std::atomic<bool> alive_{false};
  std::shared_ptr<std::atomic<int64_t>> inFlight_ =
      std::make_shared<std::atomic<int64_t>>(0);
};
