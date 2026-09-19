#pragma once

#include <nats/nats.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// NATS event bus over cnats; handlers run on cnats worker threads and must not block.
class NatsBus
{
public:
  // Delivers the concrete subject a wildcard matched plus the payload.
  using MessageHandler =
      std::function<void(std::string_view subject, std::string_view payload)>;

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

  // Connects and starts the reconnect supervisor; a failed first attempt keeps
  // retrying in the background, so handlers registered meanwhile stay pending.
  bool connect(const Options& options);
  bool connect() { return connect(optionsFromConfig()); }

  bool publish(std::string_view subject, std::string_view payload);

  struct PublishWithIdInput
  {
    std::string subject;
    std::string payload;
    std::string msgId;
  };

  // Publishes through JetStream and waits for the PubAck. False means the
  // broker did not store the message: the caller must retain and retry it.
  // Never degrades to core NATS.
  bool publishWithMsgId(const PublishWithIdInput& input);

  struct StreamInput
  {
    std::string name;
    std::vector<std::string> subjects;
    int64_t maxAgeNs{0};
    int64_t duplicatesNs{0};
  };

  // Creates the JetStream stream when missing; safe to call repeatedly.
  bool ensureStream(const StreamInput& input);

  struct StreamStatus
  {
    bool exists{false};
    std::vector<std::string> subjects;
    int64_t maxAgeNs{0};
    int64_t duplicatesNs{0};
  };

  // Reads back the live stream configuration; nullopt when the stream is
  // missing or unreachable.
  std::optional<StreamStatus> streamInfo(const std::string& name);

  struct DurableMessage
  {
    std::string_view subject;
    std::string_view payload;
    int delivered{0};
  };

  // Settle once: ack (done), nak (redeliver) or term (drop without redelivery).
  struct DurableSettlement
  {
    std::function<void()> ack;
    std::function<void()> nak;
    std::function<void()> term;
  };

  // Durable JetStream consumer; the message views stay valid only for the call.
  using DurableHandler =
      std::function<void(const DurableMessage&, DurableSettlement)>;
  struct DurableInput
  {
    std::string stream;
    std::string durable;
    std::string subject;
    bool deliverAll{false};
    int maxDeliver{5};
    DurableHandler handler;
  };
  std::optional<uint64_t> subscribeDurable(const DurableInput& input);

  // Handlers registered before connect() become pending and activate once connected.
  std::optional<uint64_t> subscribe(const std::string& subject,
                                    MessageHandler handler);
  bool unsubscribe(uint64_t id);

  // Idempotent: releases every handler, subscription and connection; safe from a message callback.
  void drain();

  bool isConnected() const;
  // Unynchronized; call from the thread that owns the bus lifecycle.
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
  struct JsCtxDeleter
  {
    void operator()(jsCtx* context) const;
  };
  struct StreamInfoDeleter
  {
    void operator()(jsStreamInfo* info) const;
  };
  using ConnectionPtr = std::unique_ptr<natsConnection, ConnectionDeleter>;
  using OptionsPtr = std::unique_ptr<natsOptions, OptionsDeleter>;
  using SubscriptionPtr = std::unique_ptr<natsSubscription, SubscriptionDeleter>;
  using JsCtxPtr = std::unique_ptr<jsCtx, JsCtxDeleter>;
  using StreamInfoPtr = std::unique_ptr<jsStreamInfo, StreamInfoDeleter>;

  struct PendingSubscription
  {
    std::string subject;
    MessageHandler handler;
  };

  struct ActiveSubscription
  {
    uint64_t id{0};
    std::string subject;
    MessageHandler handler;
    SubscriptionPtr raw;
  };

  struct DurableSubscription
  {
    uint64_t id{0};
    DurableInput input;
    SubscriptionPtr raw;
  };

  // Creates the JetStream context on first use; call with mutex_ held.
  bool ensureJetStream();

  // True only while cnats reports the connection as CONNECTED; call with mutex_ held.
  bool connectedLocked() const;

  // One connection attempt plus re-attachment of every logical subscription.
  bool connectOnce();

  void startSupervisor();
  void supervise();

  // Call with mutex_ held; the raw handle is created against connection_.
  bool attach(const PendingSubscription& pending, uint64_t id);
  bool attachDurable(const DurableInput& input, uint64_t id);
  // Call with mutex_ held; retries every durable that was not attached yet.
  void attachPendingDurable();

  // Reconciles an existing stream against the wanted config; call unlocked.
  bool reconcileStream(const StreamInput& input);

  static void onDisconnected(natsConnection* connection, void* closure);
  static void onReconnected(natsConnection* connection, void* closure);
  static void onClosed(natsConnection* connection, void* closure);
  static void onMessage(natsConnection* connection, natsSubscription* sub,
                        natsMsg* msg, void* closure);
  static void onDurableMessage(natsConnection* connection,
                               natsSubscription* sub, natsMsg* msg,
                               void* closure);

  Options options_;
  ConnectionPtr connection_;
  OptionsPtr connectionOptions_;
  JsCtxPtr js_;

  mutable std::mutex mutex_;
  uint64_t nextSubscriptionId_{1};
  std::unordered_map<uint64_t, PendingSubscription> pending_;
  // Keyed by the cnats handle so the callback can resolve its handler.
  std::unordered_map<natsSubscription*, ActiveSubscription> active_;
  std::unordered_map<natsSubscription*, DurableSubscription> durable_;
  std::unordered_map<uint64_t, DurableInput> pendingDurable_;
  std::unordered_map<uint64_t, natsSubscription*> activeIndex_;
  std::thread supervisor_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> superviseStarted_{false};
  std::atomic<bool> callbacksSuppressed_{false};
  bool connected_{false};
  bool drained_{false};
};
