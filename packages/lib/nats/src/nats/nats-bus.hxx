#pragma once

#include <nats/nats.h>

#include <atomic>
#include <condition_variable>
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

class NatsBus
{
public:
  using MessageHandler =
      std::function<void(std::string_view subject, std::string_view payload)>;

  struct Options
  {
    std::string url{"nats://127.0.0.1:4222"};
    std::string user;
    std::string password;
    int reconnectWaitMs{2000};
    int maxReconnects{60};
  };

  NatsBus() = default;
  ~NatsBus();
  NatsBus(const NatsBus&) = delete;
  NatsBus& operator=(const NatsBus&) = delete;

  static Options optionsFromConfig();

  bool connect(const Options& options);
  bool connect() { return connect(optionsFromConfig()); }

  bool publish(std::string_view subject, std::string_view payload);

  struct PublishWithIdInput
  {
    std::string subject;
    std::string payload;
    std::string msgId;
  };

  bool publishWithMsgId(const PublishWithIdInput& input);

  struct StreamInput
  {
    std::string name;
    std::vector<std::string> subjects;
    int64_t maxAgeNs{0};
    int64_t duplicatesNs{0};
  };

  bool ensureStream(const StreamInput& input);

  struct StreamStatus
  {
    bool exists{false};
    std::vector<std::string> subjects;
    int64_t maxAgeNs{0};
    int64_t duplicatesNs{0};
  };

  std::optional<StreamStatus> streamInfo(const std::string& name);

  struct DurableMessage
  {
    std::string_view subject;
    std::string_view payload;
    std::string_view msgId;
    int delivered{0};
  };

  struct DurableSettlement
  {
    std::function<void()> ack;
    std::function<void()> nak;
    std::function<void()> term;
  };

  using DurableHandler =
      std::function<void(const DurableMessage&, DurableSettlement)>;
  static constexpr int kDefaultMaxAckPending = 256;
  static constexpr int kOrderedMaxAckPending = 1;
  struct DurableInput
  {
    std::string stream;
    std::string durable;
    std::string subject;
    bool deliverAll{false};
    int maxDeliver{5};
    int maxAckPending{kDefaultMaxAckPending};
    DurableHandler handler;
  };
  std::optional<uint64_t> subscribeDurable(const DurableInput& input);

  std::optional<uint64_t> subscribe(const std::string& subject,
                                    MessageHandler handler);
  bool unsubscribe(uint64_t id);

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
  struct JsCtxDeleter
  {
    void operator()(jsCtx* context) const;
  };
  struct StreamInfoDeleter
  {
    void operator()(jsStreamInfo* info) const;
  };
  struct InboxDeleter
  {
    void operator()(natsInbox* inbox) const;
  };
  using ConnectionPtr = std::unique_ptr<natsConnection, ConnectionDeleter>;
  using OptionsPtr = std::unique_ptr<natsOptions, OptionsDeleter>;
  using SubscriptionPtr = std::unique_ptr<natsSubscription, SubscriptionDeleter>;
  using SharedSubscriptionPtr = std::shared_ptr<natsSubscription>;
  using JsCtxPtr = std::shared_ptr<jsCtx>;
  using StreamInfoPtr = std::unique_ptr<jsStreamInfo, StreamInfoDeleter>;
  using InboxPtr = std::unique_ptr<natsInbox, InboxDeleter>;

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
    SharedSubscriptionPtr raw;
  };

  bool ensureJetStream();
  [[nodiscard]] JsCtxPtr jetStream();

  bool connectedLocked() const;

  bool connectOnce();

  void startSupervisor();
  void supervise();

  bool attach(const PendingSubscription& pending, uint64_t id);
  bool ensureDurable(const DurableInput& input);
  bool attachDurable(const DurableInput& input, uint64_t id);
  void attachPendingDurable();

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
  std::condition_variable closedSignal_;
  int openConnections_{0};
  uint64_t nextSubscriptionId_{1};
  std::unordered_map<uint64_t, PendingSubscription> pending_;
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
