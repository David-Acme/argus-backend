#pragma once

#include <nats/nats.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// NATS event bus: publish/subscribe service used to fan sync-change events to
// the gateway. Handlers run on cnats worker threads and must not block; heavy
// work belongs in a BlockingTask. connect() blocks until the first connection
// result, so it must never be called on a Drogon IO thread.
class NatsBus
{
public:
  using MessageHandler = std::function<void(std::string_view)>;

  struct Options
  {
    std::string url{"nats://127.0.0.1:4222"};
    int reconnectWaitMs{2000};
    int maxReconnects{60};
  };

  NatsBus() = default;
  ~NatsBus();
  NatsBus(const NatsBus&) = delete;
  NatsBus& operator=(const NatsBus&) = delete;

  // Resolves [nats] url/reconnect_wait_ms/max_reconnects with sane defaults.
  static Options optionsFromConfig();

  // Returns false when the server is unreachable at call time.
  bool connect(const Options& options);
  bool connect() { return connect(optionsFromConfig()); }

  bool publish(std::string_view subject, std::string_view payload);

  // Handlers registered before connect() become pending and activate once the
  // connection is up.
  std::optional<uint64_t> subscribe(const std::string& subject,
                                    MessageHandler handler);
  bool unsubscribe(uint64_t id);

  // Idempotent: releases every handler, unsubscribes and closes the
  // connection. A later connect() starts a fresh lifecycle.
  void drain();

  bool isConnected() const;
  const Options& options() const { return options_; }

private:
  struct ConnectionDeleter
  {
    void operator()(natsConnection* connection) const;
  };
  struct OptionsDeleter
  {
    void operator()(natsOptions* options) const;
  };
  struct SubscriptionDeleter
  {
    void operator()(natsSubscription* subscription) const;
  };
  using ConnectionPtr = std::unique_ptr<natsConnection, ConnectionDeleter>;
  using OptionsPtr = std::unique_ptr<natsOptions, OptionsDeleter>;
  using SubscriptionPtr = std::unique_ptr<natsSubscription, SubscriptionDeleter>;

  struct PendingSubscription
  {
    std::string subject;
    MessageHandler handler;
  };

  struct ActiveSubscription
  {
    SubscriptionPtr raw;
    MessageHandler handler;
  };

  static void onDisconnected(natsConnection* connection, void* closure);
  static void onReconnected(natsConnection* connection, void* closure);
  static void onClosed(natsConnection* connection, void* closure);
  static void onMessage(natsConnection* connection, natsSubscription* sub,
                        natsMsg* msg, void* closure);

  Options options_;
  ConnectionPtr connection_;
  OptionsPtr connectionOptions_;

  mutable std::mutex mutex_;
  uint64_t nextSubscriptionId_{1};
  std::unordered_map<uint64_t, PendingSubscription> pending_;
  // Keyed by the cnats handle so the callback can resolve its handler.
  std::unordered_map<natsSubscription*, ActiveSubscription> active_;
  std::unordered_map<uint64_t, natsSubscription*> activeIndex_;
  bool connected_{false};
  bool drained_{false};
};