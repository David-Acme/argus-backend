#include "nats-bus.hxx"

#include <drogon/drogon.h>
#include <nats/nats.h>

#include <utility>
#include <vector>

#include "../../services/config-service/config-service.hxx"
#include "nats-subject.hxx"

namespace
{
constexpr int64_t kDrainTimeoutMs = 1000;
} // namespace

void NatsBus::ConnectionDeleter::operator()(natsConnection* connection) const
{
  if (connection != nullptr)
    natsConnection_Destroy(connection);
}

void NatsBus::OptionsDeleter::operator()(natsOptions* options) const
{
  if (options != nullptr)
    natsOptions_Destroy(options);
}

void NatsBus::SubscriptionDeleter::operator()(natsSubscription* subscription)
    const
{
  if (subscription != nullptr)
    natsSubscription_Destroy(subscription);
}

NatsBus::~NatsBus()
{
  drain();
}

NatsBus::Options NatsBus::optionsFromConfig()
{
  Options options;
  const std::string url = ConfigService::getString("nats.url");
  if (!url.empty())
    options.url = url;

  const int reconnectWait = ConfigService::getInt("nats.reconnect_wait_ms");
  if (reconnectWait > 0)
    options.reconnectWaitMs = reconnectWait;

  const int maxReconnects = ConfigService::getInt("nats.max_reconnects");
  if (maxReconnects > 0)
    options.maxReconnects = maxReconnects;

  return options;
}

void NatsBus::onDisconnected(natsConnection* connection, void* closure)
{
  (void)connection;
  (void)closure;
  LOG_WARN << "NATS connection lost; reconnecting per policy";
}

void NatsBus::onReconnected(natsConnection* connection, void* closure)
{
  (void)connection;
  (void)closure;
  LOG_INFO << "NATS connection re-established";
}

void NatsBus::onClosed(natsConnection* connection, void* closure)
{
  (void)connection;
  auto* bus = static_cast<NatsBus*>(closure);
  std::lock_guard lock(bus->mutex_);
  bus->connected_ = false;
}

void NatsBus::onMessage(natsConnection* connection, natsSubscription* sub,
                        natsMsg* msg, void* closure)
{
  (void)connection;
  auto* bus = static_cast<NatsBus*>(closure);
  if (bus == nullptr || msg == nullptr)
    return;

  MessageHandler handler;
  {
    std::lock_guard lock(bus->mutex_);
    const auto it = bus->active_.find(sub);
    if (it != bus->active_.end())
      handler = it->second.handler;
  }

  const char* data = natsMsg_GetData(msg);
  const int length = natsMsg_GetDataLength(msg);
  if (handler && data != nullptr && length >= 0)
    handler(natsMsg_GetSubject(msg),
            std::string_view(data, static_cast<size_t>(length)));

  // The dispatcher hands the callback ownership of the message.
  natsMsg_Destroy(msg);
}

bool NatsBus::connect(const Options& options)
{
  {
    std::lock_guard lock(mutex_);
    if (connected_)
      return true;
  }

  natsOptions* rawOptions = nullptr;
  if (natsOptions_Create(&rawOptions) != NATS_OK)
    return false;
  OptionsPtr opts(rawOptions);

  natsStatus status = natsOptions_SetURL(opts.get(), options.url.c_str());
  if (status == NATS_OK)
    status = natsOptions_SetReconnectWait(
        opts.get(), static_cast<int64_t>(options.reconnectWaitMs));
  if (status == NATS_OK)
    status = natsOptions_SetMaxReconnect(opts.get(), options.maxReconnects);
  if (status == NATS_OK)
    status = natsOptions_SetDisconnectedCB(opts.get(), onDisconnected, this);
  if (status == NATS_OK)
    status = natsOptions_SetReconnectedCB(opts.get(), onReconnected, this);
  if (status == NATS_OK)
    status = natsOptions_SetClosedCB(opts.get(), onClosed, this);
  if (status != NATS_OK) {
    LOG_FATAL << "NATS options setup failed: " << natsStatus_GetText(status);
    return false;
  }

  natsConnection* raw = nullptr;
  status = natsConnection_Connect(&raw, opts.get());
  if (status != NATS_OK) {
    LOG_WARN << "NATS connect to " << options.url << " failed: "
             << natsStatus_GetText(status);
    return false;
  }

  std::vector<SubscriptionPtr> stale;
  {
    std::lock_guard lock(mutex_);
    options_ = options;
    connectionOptions_ = std::move(opts);
    if (connection_ != nullptr) {
      // The previous connection was closed externally; release its
      // subscriptions so a re-connect starts from a clean slate.
      for (auto& [sub, active] : active_)
        stale.push_back(std::move(active.raw));
      active_.clear();
      activeIndex_.clear();
    }
    connection_.reset(raw);
    connected_ = true;
    drained_ = false;

    for (auto& [id, pending] : pending_) {
      natsSubscription* rawSub = nullptr;
      const natsStatus subStatus =
          natsConnection_Subscribe(&rawSub, connection_.get(),
                                   pending.subject.c_str(), onMessage, this);
      if (subStatus == NATS_OK && rawSub != nullptr) {
        active_.emplace(rawSub,
                        ActiveSubscription{SubscriptionPtr(rawSub),
                                           pending.handler});
        activeIndex_.emplace(id, rawSub);
      } else {
        LOG_WARN << "NATS subscription to " << pending.subject
                 << " failed: " << natsStatus_GetText(subStatus);
      }
    }
    pending_.clear();
  }

  for (auto& sub : stale)
    natsSubscription_Unsubscribe(sub.get());
  return true;
}

bool NatsBus::publish(std::string_view subject, std::string_view payload)
{
  if (!nats_subject::isValidSubject(subject,
                                    nats_subject::SubjectKind::Publish))
    return false;

  std::lock_guard lock(mutex_);
  if (!connected_ || connection_ == nullptr)
    return false;

  const natsStatus status = natsConnection_Publish(
      connection_.get(), std::string(subject).c_str(), payload.data(),
      static_cast<int>(payload.size()));
  if (status != NATS_OK) {
    LOG_WARN << "NATS publish to " << subject
             << " failed: " << natsStatus_GetText(status);
    return false;
  }
  return true;
}

std::optional<uint64_t> NatsBus::subscribe(const std::string& subject,
                                           MessageHandler handler)
{
  if (!nats_subject::isValidSubject(subject,
                                    nats_subject::SubjectKind::Subscribe))
    return std::nullopt;

  std::lock_guard lock(mutex_);
  if (drained_)
    return std::nullopt;

  const uint64_t id = nextSubscriptionId_++;
  if (connected_ && connection_ != nullptr) {
    natsSubscription* rawSub = nullptr;
    const natsStatus status =
        natsConnection_Subscribe(&rawSub, connection_.get(), subject.c_str(),
                                 onMessage, this);
    if (status != NATS_OK || rawSub == nullptr) {
      LOG_WARN << "NATS subscription to " << subject
               << " failed: " << natsStatus_GetText(status);
      return std::nullopt;
    }
    active_.emplace(rawSub, ActiveSubscription{SubscriptionPtr(rawSub),
                                               std::move(handler)});
    activeIndex_.emplace(id, rawSub);
    return id;
  }

  pending_.emplace(id, PendingSubscription{subject, std::move(handler)});
  return id;
}

bool NatsBus::unsubscribe(uint64_t id)
{
  SubscriptionPtr raw;
  {
    std::lock_guard lock(mutex_);
    const auto index = activeIndex_.find(id);
    if (index == activeIndex_.end()) {
      pending_.erase(id);
      return true;
    }
    const auto it = active_.find(index->second);
    if (it != active_.end()) {
      raw = std::move(it->second.raw);
      active_.erase(it);
    }
    activeIndex_.erase(index);
  }

  natsSubscription_Unsubscribe(raw.get());
  return true;
}

void NatsBus::drain()
{
  std::vector<SubscriptionPtr> subs;
  ConnectionPtr connection;
  OptionsPtr options;
  {
    std::lock_guard lock(mutex_);
    if (drained_)
      return;
    drained_ = true;
    connected_ = false;
    pending_.clear();
    subs.reserve(active_.size());
    for (auto& [sub, active] : active_)
      subs.push_back(std::move(active.raw));
    active_.clear();
    activeIndex_.clear();
    connection = std::move(connection_);
    options = std::move(connectionOptions_);
  }

  for (auto& sub : subs)
    natsSubscription_Unsubscribe(sub.get());
  subs.clear();

  if (connection != nullptr)
    natsConnection_DrainTimeout(connection.get(), kDrainTimeoutMs);
}

bool NatsBus::isConnected() const
{
  std::lock_guard lock(mutex_);
  return connected_;
}
