#include "nats-bus.hxx"

#include <drogon/drogon.h>
#include <nats/nats.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

#include "../../services/config-service/config-service.hxx"
#include "nats-subject.hxx"

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

void NatsBus::JsCtxDeleter::operator()(jsCtx* context) const
{
  if (context != nullptr)
    jsCtx_Destroy(context);
}

void NatsBus::StreamInfoDeleter::operator()(jsStreamInfo* info) const
{
  if (info != nullptr)
    jsStreamInfo_Destroy(info);
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
  auto* bus = static_cast<NatsBus*>(closure);
  if (bus == nullptr ||
      bus->callbacksSuppressed_.load(std::memory_order_acquire))
    return;
  std::lock_guard lock(bus->mutex_);
  if (bus->connection_.get() != connection ||
      bus->callbacksSuppressed_.load(std::memory_order_acquire))
    return;
  LOG_WARN << "NATS connection lost; reconnecting per policy";
}

void NatsBus::onReconnected(natsConnection* connection, void* closure)
{
  auto* bus = static_cast<NatsBus*>(closure);
  if (bus == nullptr ||
      bus->callbacksSuppressed_.load(std::memory_order_acquire))
    return;
  std::lock_guard lock(bus->mutex_);
  if (bus->connection_.get() != connection ||
      bus->callbacksSuppressed_.load(std::memory_order_acquire))
    return;
  LOG_INFO << "NATS connection re-established";
}

void NatsBus::onClosed(natsConnection* connection, void* closure)
{
  auto* bus = static_cast<NatsBus*>(closure);
  if (bus == nullptr ||
      bus->callbacksSuppressed_.load(std::memory_order_acquire))
    return;
  std::lock_guard lock(bus->mutex_);
  if (bus->connection_.get() != connection ||
      bus->callbacksSuppressed_.load(std::memory_order_acquire))
    return;
  bus->connected_ = false;
}

void NatsBus::onMessage(natsConnection* connection, natsSubscription* sub,
                        natsMsg* msg, void* closure)
{
  (void)connection;
  auto* bus = static_cast<NatsBus*>(closure);
  if (bus == nullptr || msg == nullptr ||
      bus->callbacksSuppressed_.load(std::memory_order_acquire))
    return;

  MessageHandler handler;
  {
    std::lock_guard lock(bus->mutex_);
    if (!bus->callbacksSuppressed_.load(std::memory_order_acquire)) {
      const auto it = bus->active_.find(sub);
      if (it != bus->active_.end())
        handler = it->second.handler;
    }
  }

  const char* data = natsMsg_GetData(msg);
  const int length = natsMsg_GetDataLength(msg);
  if (handler && data != nullptr && length >= 0)
    handler(natsMsg_GetSubject(msg),
            std::string_view(data, static_cast<size_t>(length)));

  natsMsg_Destroy(msg);
}

void NatsBus::onDurableMessage(natsConnection* connection,
                               natsSubscription* sub, natsMsg* msg,
                               void* closure)
{
  (void)connection;
  auto* bus = static_cast<NatsBus*>(closure);
  if (bus == nullptr || msg == nullptr ||
      bus->callbacksSuppressed_.load(std::memory_order_acquire)) {
    if (msg != nullptr)
      natsMsg_Destroy(msg);
    return;
  }

  DurableHandler handler;
  {
    std::lock_guard lock(bus->mutex_);
    if (!bus->callbacksSuppressed_.load(std::memory_order_acquire)) {
      const auto it = bus->durable_.find(sub);
      if (it != bus->durable_.end())
        handler = it->second.input.handler;
    }
  }

  if (!handler) {
    natsMsg_Destroy(msg);
    return;
  }

  const std::shared_ptr<natsMsg> message(
      msg, [](natsMsg* value) { natsMsg_Destroy(value); });
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto ack = [message, settled]() {
    if (settled->exchange(true, std::memory_order_acq_rel))
      return;
    natsMsg_Ack(message.get(), nullptr);
  };
  auto nak = [message, settled]() {
    if (settled->exchange(true, std::memory_order_acq_rel))
      return;
    natsMsg_Nak(message.get(), nullptr);
  };
  auto term = [message, settled]() {
    if (settled->exchange(true, std::memory_order_acq_rel))
      return;
    natsMsg_Term(message.get(), nullptr);
  };
  int delivered = 0;
  jsMsgMetaData* meta = nullptr;
  if (natsMsg_GetMetaData(&meta, msg) == NATS_OK && meta != nullptr) {
    delivered = static_cast<int>(meta->NumDelivered);
    jsMsgMetaData_Destroy(meta);
  }
  const DurableMessage durableMessage{
      .subject = natsMsg_GetSubject(msg),
      .payload = std::string_view(
          natsMsg_GetData(msg),
          static_cast<size_t>(natsMsg_GetDataLength(msg))),
      .delivered = delivered};
  handler(durableMessage, DurableSettlement{.ack = std::move(ack),
                                            .nak = std::move(nak),
                                            .term = std::move(term)});
}

bool NatsBus::connect(const Options& options)
{
  {
    std::lock_guard lock(mutex_);
    if (drained_)
      return false;
    options_ = options;
    if (connected_ && connection_ != nullptr)
      return true;
  }
  callbacksSuppressed_.store(false, std::memory_order_release);
  const bool connected = connectOnce();
  startSupervisor();
  return connected;
}

bool NatsBus::connectedLocked() const
{
  if (!connected_ || connection_ == nullptr)
    return false;
  const natsConnStatus status = natsConnection_Status(connection_.get());
  return status == NATS_CONN_STATUS_CONNECTED;
}

bool NatsBus::connectOnce()
{
  Options options;
  {
    std::lock_guard lock(mutex_);
    if (drained_)
      return false;
    if (connection_ != nullptr) {
      const natsConnStatus status = natsConnection_Status(connection_.get());
      if (status != NATS_CONN_STATUS_CLOSED &&
          status != NATS_CONN_STATUS_DISCONNECTED &&
          status != NATS_CONN_STATUS_DRAINING_PUBS &&
          status != NATS_CONN_STATUS_DRAINING_SUBS)
        return true;
    }
    options = options_;
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
    LOG_WARN << "NATS options setup failed: " << natsStatus_GetText(status);
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
  std::unordered_map<natsSubscription*, ActiveSubscription> oldActive;
  std::unordered_map<natsSubscription*, DurableSubscription> oldDurable;
  {
    std::lock_guard lock(mutex_);
    if (drained_) {
      ConnectionPtr(raw).reset();
      return false;
    }
    connectionOptions_ = std::move(opts);
    js_.reset();
    oldActive = std::move(active_);
    oldDurable = std::move(durable_);
    active_.clear();
    durable_.clear();
    pendingDurable_.clear();
    activeIndex_.clear();
    connection_.reset(raw);
    connected_ = true;
    ensureJetStream();

    for (auto& [sub, active] : oldActive)
      stale.push_back(std::move(active.raw));
    for (auto& [sub, durable] : oldDurable) {
      stale.push_back(std::move(durable.raw));
      pendingDurable_.emplace(durable.id, durable.input);
    }

    for (const auto& [id, pending] : pending_) {
      if (pendingDurable_.contains(id))
        continue;
      attach(pending, id);
    }
    pending_.clear();
    for (const auto& [sub, active] : oldActive)
      attach({.subject = active.subject, .handler = active.handler},
             active.id);
    attachPendingDurable();
  }

  for (auto& sub : stale)
    natsSubscription_Unsubscribe(sub.get());
  return true;
}

bool NatsBus::attach(const PendingSubscription& pending, uint64_t id)
{
  natsSubscription* rawSub = nullptr;
  const natsStatus status =
      natsConnection_Subscribe(&rawSub, connection_.get(),
                               pending.subject.c_str(), onMessage, this);
  if (status != NATS_OK || rawSub == nullptr) {
    LOG_WARN << "NATS subscription to " << pending.subject
             << " failed: " << natsStatus_GetText(status);
    return false;
  }
  active_.emplace(rawSub, ActiveSubscription{.id = id,
                                             .subject = pending.subject,
                                             .handler = pending.handler,
                                             .raw = SubscriptionPtr(rawSub)});
  activeIndex_.emplace(id, rawSub);
  return true;
}

bool NatsBus::attachDurable(const DurableInput& input, uint64_t id)
{
  if (js_ == nullptr)
    return false;

  jsOptions options;
  jsOptions_Init(&options);
  jsSubOptions subOptions;
  jsSubOptions_Init(&subOptions);
  subOptions.Stream = input.stream.c_str();
  subOptions.Config.Durable = input.durable.c_str();
  subOptions.Config.DeliverPolicy =
      input.deliverAll ? js_DeliverAll : js_DeliverNew;
  subOptions.Config.AckPolicy = js_AckExplicit;
  subOptions.Config.AckWait = 60LL * 1000 * 1000 * 1000;
  subOptions.Config.MaxDeliver = input.maxDeliver > 0 ? input.maxDeliver : 5;
  subOptions.ManualAck = true;

  natsSubscription* rawSub = nullptr;
  jsErrCode errorCode = jsErrCode(0);
  const natsStatus status =
      js_Subscribe(&rawSub, js_.get(), input.subject.c_str(), onDurableMessage,
                   this, &options, &subOptions, &errorCode);
  if (status != NATS_OK || rawSub == nullptr) {
    LOG_WARN << "NATS durable subscribe to " << input.subject
             << " failed: " << natsStatus_GetText(status) << " (err "
             << errorCode << ")";

    return false;
  }
  durable_.emplace(rawSub, DurableSubscription{.id = id,
                                               .input = input,
                                               .raw = SubscriptionPtr(rawSub)});
  activeIndex_.emplace(id, rawSub);
  return true;
}

void NatsBus::attachPendingDurable()
{
  for (auto it = pendingDurable_.begin(); it != pendingDurable_.end();) {
    if (attachDurable(it->second, it->first))
      it = pendingDurable_.erase(it);
    else
      ++it;
  }
}

void NatsBus::startSupervisor()
{
  if (superviseStarted_.exchange(true, std::memory_order_acq_rel))
    return;
  supervisor_ = std::thread([this]() { supervise(); });
}

void NatsBus::supervise()
{
  int backoffMs = 500;
  bool awaitingClientReconnect = false;
  while (!stopping_.load(std::memory_order_acquire)) {
    {
      std::lock_guard lock(mutex_);
      if (drained_)
        return;
      if (connectedLocked()) {
        awaitingClientReconnect = false;
        if (!pendingDurable_.empty() && ensureJetStream())
          attachPendingDurable();
      }
    }
    if (isConnected()) {
      backoffMs = 500;
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }
    if (connectOnce()) {
      if (!awaitingClientReconnect) {
        LOG_WARN << "NATS bus reconnecting; waiting for the connection";
        awaitingClientReconnect = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }
    awaitingClientReconnect = false;
    LOG_WARN << "NATS bus connection retry failed";
    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
    backoffMs = std::min(backoffMs * 2, 30000);
  }
}

bool NatsBus::publish(std::string_view subject, std::string_view payload)
{
  if (!nats_subject::isValidSubject(subject,
                                    nats_subject::SubjectKind::Publish))
    return false;

  std::lock_guard lock(mutex_);
  if (!connectedLocked())
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

bool NatsBus::ensureJetStream()
{
  if (js_ != nullptr)
    return true;
  if (!connectedLocked())
    return false;
  jsCtx* raw = nullptr;
  const natsStatus status =
      natsConnection_JetStream(&raw, connection_.get(), nullptr);
  if (status != NATS_OK || raw == nullptr) {
    LOG_WARN << "NATS JetStream context failed: " << natsStatus_GetText(status);
    return false;
  }
  js_.reset(raw);
  return true;
}

bool NatsBus::publishWithMsgId(const PublishWithIdInput& input)
{
  if (!nats_subject::isValidSubject(input.subject,
                                    nats_subject::SubjectKind::Publish) ||
      input.msgId.empty())
    return false;

  std::lock_guard lock(mutex_);
  if (!connectedLocked() || !ensureJetStream())
    return false;

  jsPubOptions options;
  jsPubOptions_Init(&options);
  options.MsgId = input.msgId.c_str();
  options.MaxWait = 2000;
  jsPubAck* rawAck = nullptr;
  const natsStatus status =
      js_Publish(&rawAck, js_.get(), input.subject.c_str(), input.payload.data(),
                 static_cast<int>(input.payload.size()), &options, nullptr);
  if (rawAck != nullptr)
    jsPubAck_Destroy(rawAck);
  if (status != NATS_OK) {
    LOG_WARN << "JetStream publish to " << input.subject << " (id "
             << input.msgId
             << ") failed: " << natsStatus_GetText(status)
             << "; message not stored";
    return false;
  }
  return true;
}

bool NatsBus::ensureStream(const StreamInput& input)
{
  if (input.name.empty() || input.subjects.empty())
    return false;

  {
    std::lock_guard lock(mutex_);
    if (!connectedLocked() || !ensureJetStream())
      return false;

    std::vector<const char*> subjects;
    subjects.reserve(input.subjects.size());
    for (const auto& subject : input.subjects)
      subjects.push_back(subject.c_str());

    jsStreamConfig config;
    jsStreamConfig_Init(&config);
    config.Name = input.name.c_str();
    config.Subjects = subjects.data();
    config.SubjectsLen = static_cast<int>(subjects.size());
    config.Retention = js_LimitsPolicy;
    config.MaxAge = input.maxAgeNs;
    config.Storage = js_FileStorage;
    config.Duplicates = input.duplicatesNs;

    jsErrCode errorCode = jsErrCode(0);
    const natsStatus status =
        js_AddStream(nullptr, js_.get(), &config, nullptr, &errorCode);
    if (status == NATS_OK)
      return true;
    if (errorCode != JSStreamNameExistErr) {
      LOG_WARN << "JetStream stream " << input.name << " ensure failed (status="
               << status << " err=" << errorCode << ")";
      return false;
    }
  }
  return reconcileStream(input);
}

std::optional<NatsBus::StreamStatus>
NatsBus::streamInfo(const std::string& name)
{
  if (name.empty())
    return std::nullopt;
  std::lock_guard lock(mutex_);
  if (!connectedLocked() || !ensureJetStream())
    return std::nullopt;
  jsStreamInfo* rawInfo = nullptr;
  jsErrCode errorCode = jsErrCode(0);
  if (js_GetStreamInfo(&rawInfo, js_.get(), name.c_str(), nullptr,
                       &errorCode) != NATS_OK ||
      rawInfo == nullptr || rawInfo->Config == nullptr) {
    StreamInfoPtr(rawInfo).reset();
    return std::nullopt;
  }
  const StreamInfoPtr info(rawInfo);
  StreamStatus status;
  status.exists = true;
  for (int index = 0; index < info->Config->SubjectsLen; ++index) {
    if (info->Config->Subjects[index] != nullptr)
      status.subjects.emplace_back(info->Config->Subjects[index]);
  }
  status.maxAgeNs = info->Config->MaxAge;
  status.duplicatesNs = info->Config->Duplicates;
  return status;
}

bool NatsBus::reconcileStream(const StreamInput& input)
{
  const auto existing = streamInfo(input.name);
  if (!existing.has_value()) {
    LOG_WARN << "JetStream stream " << input.name
             << " exists but cannot be inspected; refusing to assume it";
    return false;
  }
  std::vector<std::string> wanted = input.subjects;
  std::vector<std::string> current = existing->subjects;
  std::sort(wanted.begin(), wanted.end());
  std::sort(current.begin(), current.end());
  if (wanted != current) {
    LOG_WARN << "JetStream stream " << input.name
             << " carries different subjects; refusing to repurpose it";
    return false;
  }
  if (existing->maxAgeNs == input.maxAgeNs &&
      existing->duplicatesNs == input.duplicatesNs)
    return true;

  std::lock_guard lock(mutex_);
  if (!connectedLocked() || !ensureJetStream())
    return false;
  jsStreamInfo* rawInfo = nullptr;
  jsErrCode errorCode = jsErrCode(0);
  if (js_GetStreamInfo(&rawInfo, js_.get(), input.name.c_str(), nullptr,
                       &errorCode) != NATS_OK ||
      rawInfo == nullptr || rawInfo->Config == nullptr) {
    StreamInfoPtr(rawInfo).reset();
    LOG_WARN << "JetStream stream " << input.name
             << " vanished during reconcile";
    return false;
  }
  const StreamInfoPtr info(rawInfo);
  if (info->Config->Retention != js_LimitsPolicy ||
      info->Config->Storage != js_FileStorage) {
    LOG_WARN << "JetStream stream " << input.name
             << " has incompatible retention/storage; refusing to rewrite it";
    return false;
  }
  std::vector<const char*> subjects;
  subjects.reserve(input.subjects.size());
  for (const auto& subject : input.subjects)
    subjects.push_back(subject.c_str());
  jsStreamConfig config;
  jsStreamConfig_Init(&config);
  config.Name = input.name.c_str();
  config.Subjects = subjects.data();
  config.SubjectsLen = static_cast<int>(subjects.size());
  config.Retention = js_LimitsPolicy;
  config.MaxAge = input.maxAgeNs;
  config.Storage = js_FileStorage;
  config.Duplicates = input.duplicatesNs;
  const natsStatus status =
      js_UpdateStream(nullptr, js_.get(), &config, nullptr, &errorCode);
  if (status != NATS_OK) {
    LOG_WARN << "JetStream stream " << input.name << " update failed (status="
             << status << " err=" << errorCode << ")";
    return false;
  }
  return true;
}

std::optional<uint64_t> NatsBus::subscribeDurable(const DurableInput& input)
{
  if (!nats_subject::isValidSubject(input.subject,
                                    nats_subject::SubjectKind::Subscribe) ||
      input.stream.empty() || input.durable.empty())
    return std::nullopt;

  std::lock_guard lock(mutex_);
  if (drained_ || !connectedLocked())
    return std::nullopt;

  if (!ensureJetStream())
    return std::nullopt;

  const uint64_t id = nextSubscriptionId_++;
  if (!attachDurable(input, id))
    return std::nullopt;
  return id;
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
  if (connectedLocked()) {
    if (attach({.subject = subject, .handler = std::move(handler)}, id))
      return id;
    return std::nullopt;
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
      pendingDurable_.erase(id);
      return true;
    }
    const auto it = active_.find(index->second);
    if (it != active_.end()) {
      raw = std::move(it->second.raw);
      active_.erase(it);
    }
    const auto durable = durable_.find(index->second);
    if (durable != durable_.end()) {
      raw = std::move(durable->second.raw);
      durable_.erase(durable);
    }
    activeIndex_.erase(index);
  }

  natsSubscription_Unsubscribe(raw.get());
  return true;
}

void NatsBus::drain()
{
  callbacksSuppressed_.store(true, std::memory_order_release);
  stopping_.store(true, std::memory_order_release);
  std::thread supervisor;
  std::vector<SubscriptionPtr> subs;
  ConnectionPtr connection;
  OptionsPtr options;
  JsCtxPtr js;
  {
    std::lock_guard lock(mutex_);
    if (drained_)
      return;
    drained_ = true;
    connected_ = false;
    pending_.clear();
    pendingDurable_.clear();
    subs.reserve(active_.size() + durable_.size());
    for (auto& [sub, active] : active_)
      subs.push_back(std::move(active.raw));
    for (auto& [sub, durable] : durable_)
      subs.push_back(std::move(durable.raw));
    active_.clear();
    durable_.clear();
    activeIndex_.clear();
    connection = std::move(connection_);
    options = std::move(connectionOptions_);
    js = std::move(js_);
    supervisor = std::move(supervisor_);
  }

  if (supervisor.joinable() &&
      supervisor.get_id() != std::this_thread::get_id())
    supervisor.join();

  for (auto& sub : subs)
    natsSubscription_Unsubscribe(sub.get());
  subs.clear();

  if (connection != nullptr)
    natsConnection_Close(connection.get());
}

bool NatsBus::isConnected() const
{
  std::lock_guard lock(mutex_);
  return connectedLocked();
}
