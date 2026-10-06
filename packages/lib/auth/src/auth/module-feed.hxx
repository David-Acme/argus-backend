#pragma once

#include <auth/module-gate.hxx>
#include <nats/nats-bus.hxx>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

enum class ModuleFeedDisposition : uint8_t
{
  Applied = 0,
  Ignored,
  Refused
};

class ModuleFeed
{
public:
  struct Snapshot
  {
    ModuleFlags flags;
    int64_t version{0};
    std::string epoch{};
  };

  using BootRead = std::function<std::optional<Snapshot>()>;

  struct Dependencies
  {
    std::shared_ptr<NatsBus> bus;
    ModuleGate* gate{nullptr};
    BootRead bootRead;
  };

  struct Config
  {
    std::string stream;
    std::string subject;
    std::string durable;
    std::string stateFile;
    int maxDeliver{10};
    double retrySeconds{5.0};
    int bootAttempts{12};
  };

  [[nodiscard]] static Config defaults(std::string_view service);

  ModuleFeed(Dependencies dependencies, Config config);
  ~ModuleFeed();

  ModuleFeed(const ModuleFeed&) = delete;
  ModuleFeed& operator=(const ModuleFeed&) = delete;

  void restore();
  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

  ModuleFeedDisposition handle(std::string_view body);
  bool applyAuthoritative(const Snapshot& snapshot);
  [[nodiscard]] int64_t version() const;
  [[nodiscard]] std::string epoch() const;

private:
  struct Application
  {
    bool applied{false};
    bool epochChanged{false};
  };

  [[nodiscard]] Application apply(const Snapshot& snapshot);
  void repull();
  [[nodiscard]] bool subscribe();
  void run(const std::stop_token& stop);

  Dependencies dependencies_;
  Config config_;
  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  std::optional<uint64_t> subscription_;
  mutable std::mutex applyMutex_;
  int64_t version_{0};
  std::string epoch_;
  std::atomic<int64_t> inFlight_{0};
  std::atomic<bool> connecting_{false};
  std::jthread connector_;
};

struct ModuleGateInstall
{
  std::string service;
  std::shared_ptr<NatsBus> bus;
};

namespace module_gate
{
[[nodiscard]] ModuleFeed::BootRead settingsBootRead();
[[nodiscard]] std::unique_ptr<ModuleFeed> install(const ModuleGateInstall& input);
}
