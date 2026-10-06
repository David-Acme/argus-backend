#pragma once

#include <feature/pending-intent/services/pending-intent-service.hxx>
#include <nats/nats-bus.hxx>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct ModuleSignal
{
  enum class Kind : std::uint8_t
  {
    Activated,
    Failed
  };

  Kind kind{Kind::Activated};
  std::string module;
  std::string reason;
};

struct ModuleIntentFeedConfig
{
  std::string stream;
  std::string subject;
  std::string durable;
  int maxDeliver{10};
  double retrySeconds{5.0};
};

class ModuleIntentFeed
{
public:
  ModuleIntentFeed(std::shared_ptr<NatsBus> bus, PendingIntentService& service, ModuleIntentFeedConfig config);
  ~ModuleIntentFeed();

  ModuleIntentFeed(const ModuleIntentFeed&) = delete;
  ModuleIntentFeed& operator=(const ModuleIntentFeed&) = delete;

  [[nodiscard]] static std::vector<ModuleSignal> decode(std::string_view payload);

  [[nodiscard]] static ModuleIntentFeedConfig defaults();

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  void run(const std::stop_token& stop);
  [[nodiscard]] bool subscribe();

  std::shared_ptr<NatsBus> bus_;
  PendingIntentService& service_;
  ModuleIntentFeedConfig config_;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::optional<uint64_t> subscription_;
  std::atomic<int64_t> inFlight_{0};
  std::atomic<bool> connecting_{false};
  std::jthread connector_;
};
