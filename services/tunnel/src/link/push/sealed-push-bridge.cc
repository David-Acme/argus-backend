#include <link/push/sealed-push-bridge.hxx>

#include <drogon/drogon.h>
#include <json/value.h>
#include <nats/nats-subject.hxx>
#include <text/base64.hxx>
#include <text/json-util.hxx>

#include <chrono>
#include <string_view>
#include <utility>

namespace tunnel::link
{
namespace
{
std::string hexId(std::string_view bytes)
{
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    hex.push_back(kDigits[byte >> 4U]);
    hex.push_back(kDigits[byte & 0x0FU]);
  }
  return hex;
}

std::optional<std::string> textField(const Json::Value& json, const char* key)
{
  if (!json.isMember(key) || !json[key].isString())
    return std::nullopt;
  return json[key].asString();
}

std::optional<std::string> bytesField(const Json::Value& json, const char* key)
{
  const auto text = textField(json, key);
  if (!text)
    return std::nullopt;
  return base64::decode(*text);
}

std::optional<uint32_t> ttlField(const Json::Value& json)
{
  if (!json.isMember("ttlSeconds") || !json["ttlSeconds"].isIntegral())
    return std::nullopt;
  const auto ttl = json["ttlSeconds"].asInt64();
  if (ttl < 0 || ttl > static_cast<int64_t>(UINT32_MAX))
    return std::nullopt;
  return static_cast<uint32_t>(ttl);
}

std::optional<int64_t> expiresAtField(const Json::Value& json)
{
  if (!json.isMember("expiresAt") || !json["expiresAt"].isIntegral())
    return std::nullopt;
  const auto expiresAt = json["expiresAt"].asInt64();
  if (expiresAt < 0)
    return std::nullopt;
  return expiresAt;
}

int64_t epochMilliseconds()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

namespace sealed_push_json
{
std::optional<DurableDelivery> deliveryFromJson(const std::string& text)
{
  const Json::Value json = json_util::fromString(text);
  if (!json.isObject())
    return std::nullopt;
  const auto provider = textField(json, "provider");
  const auto kind = textField(json, "kind");
  const auto priority = textField(json, "priority");
  const auto ttl = ttlField(json);
  const auto expiresAt = expiresAtField(json);
  const auto deliveryId = bytesField(json, "deliveryId");
  const auto token = textField(json, "token");
  const auto collapseId = textField(json, "collapseId");
  const auto enc = bytesField(json, "enc");
  const auto ciphertext = bytesField(json, "ciphertext");
  if (!provider || !kind || !priority || !ttl || !expiresAt || !deliveryId || !token ||
      !collapseId || !enc || !ciphertext)
    return std::nullopt;
  const auto parsedProvider = pushProviderFromString(*provider);
  const auto parsedKind = pushKindFromString(*kind);
  const auto parsedPriority = pushPriorityFromString(*priority);
  if (!parsedProvider || !parsedKind || !parsedPriority)
    return std::nullopt;
  PushSealedEnvelope envelope{.deliveryId = *deliveryId,
                              .provider = *parsedProvider,
                              .kind = *parsedKind,
                              .priority = *parsedPriority,
                              .ttlSeconds = *ttl,
                              .collapseId = *collapseId,
                              .token = *token,
                              .enc = *enc,
                              .ciphertext = *ciphertext};
  if (!push_envelope::wellFormed(envelope))
    return std::nullopt;
  return DurableDelivery{.envelope = std::move(envelope), .expiresAtMs = *expiresAt};
}

std::string resultToJson(const PushResultEnvelope& result, std::string_view token)
{
  Json::Value json(Json::objectValue);
  json["deliveryId"] = base64::encode(result.deliveryId);
  json["outcome"] = std::string(pushOutcomeToString(result.outcome));
  json["token"] = std::string(token);
  return json_util::toString(json);
}
}

SealedPushLedger::SealedPushLedger(size_t capacity) : capacity_(capacity) {}

bool SealedPushLedger::contains(const std::string& deliveryId) const
{
  return pending_.contains(deliveryId);
}

void SealedPushLedger::remember(const std::string& deliveryId, Pending pending)
{
  if (pending_.contains(deliveryId))
    return;
  while (order_.size() >= capacity_) {
    pending_.erase(order_.front());
    order_.pop_front();
  }
  order_.push_back(deliveryId);
  pending_.emplace(deliveryId, std::move(pending));
}

void SealedPushLedger::replaceSettlement(const std::string& deliveryId, NatsBus::DurableSettlement settlement)
{
  const auto found = pending_.find(deliveryId);
  if (found != pending_.end())
    found->second.settlement = std::move(settlement);
}

std::optional<SealedPushLedger::Pending> SealedPushLedger::take(const std::string& deliveryId)
{
  const auto found = pending_.find(deliveryId);
  if (found == pending_.end())
    return std::nullopt;
  Pending pending = std::move(found->second);
  pending_.erase(found);
  return pending;
}

DeliveredRecently::DeliveredRecently(size_t capacity) : capacity_(capacity) {}

bool DeliveredRecently::contains(const std::string& deliveryId) const
{
  return seen_.contains(deliveryId);
}

void DeliveredRecently::remember(const std::string& deliveryId)
{
  if (seen_.contains(deliveryId))
    return;
  while (order_.size() >= capacity_) {
    seen_.erase(order_.front());
    order_.pop_front();
  }
  order_.push_back(deliveryId);
  seen_.insert(deliveryId);
}

SealedPushDispatcher::SealedPushDispatcher(Ports ports, size_t capacity)
    : ports_(std::move(ports)), pending_(capacity), delivered_(capacity)
{
}

DispatchOutcome SealedPushDispatcher::dispatch(const DurableDelivery& delivery,
                                               NatsBus::DurableSettlement settlement)
{
  const std::string& deliveryId = delivery.envelope.deliveryId;
  if (delivery.expiresAtMs <= ports_.nowMs()) {
    ++expired_;
    LOG_INFO << "argus-tunnel: a sealed push past its end was acked without sending ("
             << expired_ << " total)";
    settlement.ack();
    return DispatchOutcome::Expired;
  }
  if (delivered_.contains(deliveryId)) {
    LOG_INFO << "argus-tunnel: sealed push " << hexId(deliveryId) << " redelivered after its result, acked";
    settlement.ack();
    return DispatchOutcome::Duplicate;
  }
  if (pending_.contains(deliveryId)) {
    LOG_INFO << "argus-tunnel: sealed push " << hexId(deliveryId) << " redelivered while pending, waiting for its result";
    pending_.replaceSettlement(deliveryId, std::move(settlement));
    return DispatchOutcome::Duplicate;
  }
  const uint32_t attempt = ++attempts_[deliveryId];
  if (!ports_.send(delivery.envelope)) {
    LOG_WARN << "argus-tunnel: sealed push " << hexId(deliveryId) << " not sent, link down (attempt " << attempt << "), nak";
    settlement.nak();
    return DispatchOutcome::LinkDown;
  }
  LOG_INFO << "argus-tunnel: sealed push " << hexId(deliveryId) << " sent to the push service (attempt " << attempt << ")";
  pending_.remember(deliveryId, {.token = delivery.envelope.token, .settlement = std::move(settlement)});
  return DispatchOutcome::Sent;
}

void SealedPushDispatcher::result(const PushResultEnvelope& outcome)
{
  auto pending = pending_.take(outcome.deliveryId);
  const std::string token = pending ? pending->token : std::string{};
  const bool published = ports_.publishResult(outcome, token);
  const uint32_t attempt = attempts_.contains(outcome.deliveryId) ? attempts_[outcome.deliveryId] : 0;
  attempts_.erase(outcome.deliveryId);
  if (!pending)
    return;
  LOG_INFO << "argus-tunnel: sealed push " << hexId(outcome.deliveryId) << " result published=" << published
           << " (attempt " << attempt << ")";
  if (!published) {
    pending->settlement.nak();
    return;
  }
  delivered_.remember(outcome.deliveryId);
  pending->settlement.ack();
}

ConsumerEdge DurableConsumerState::observe(bool attached)
{
  if (attached && !ready_) {
    ready_ = true;
    return ConsumerEdge::Ready;
  }
  if (!attached && ready_) {
    ready_ = false;
    return ConsumerEdge::Lost;
  }
  return ConsumerEdge::None;
}

std::string consumerReadyLine(std::string_view subject, std::string_view consumer)
{
  return "sealed push bridge ready on " + std::string(subject) + ", consumer " + std::string(consumer);
}

std::string consumerLostLine(std::string_view subject, std::string_view consumer)
{
  return "sealed push bridge lost " + std::string(subject) + ", consumer " + std::string(consumer);
}

SealedPushBridge::SealedPushBridge(PollLoop& loop, std::shared_ptr<NatsBus> bus)
    : loop_(loop),
      bus_(std::move(bus)),
      dispatcher_(std::make_unique<SealedPushDispatcher>(
          SealedPushDispatcher::Ports{
              .send =
                  [this](const PushSealedEnvelope& envelope) {
                    return client_ != nullptr && client_->sendSealedPush(envelope);
                  },
              .publishResult =
                  [this](const PushResultEnvelope& result, std::string_view token) {
                    return bus_ != nullptr &&
                           bus_->publish(nats_subject::kNotificationPushResult,
                                         sealed_push_json::resultToJson(result, token));
                  },
              .nowMs = epochMilliseconds},
          kLedgerCapacity))
{
}

void SealedPushBridge::attach(LinkClient& client)
{
  client_ = &client;
}

NatsBus::DurableInput SealedPushBridge::consumerInput()
{
  return {.stream = nats_subject::kNotificationPushStream,
          .durable = nats_subject::kTunnelPushConsumer,
          .subject = nats_subject::kNotificationPushDelivery,
          .deliverAll = true,
          .quiet = true,
          .maxDeliver = kMaxDeliver,
          .maxAckPending = NatsBus::kOrderedMaxAckPending,
          .handler = [this](const NatsBus::DurableMessage& message,
                            NatsBus::DurableSettlement settlement) {
            onDelivery(message, std::move(settlement));
          }};
}

void SealedPushBridge::tick()
{
  if (!bus_)
    return;
  if (!consumer_)
    consumer_ = bus_->subscribeDurable(consumerInput());
  const bool attached = consumer_.has_value() && bus_->durableAttached(*consumer_);
  switch (consumerState_.observe(attached)) {
    case ConsumerEdge::Ready:
      LOG_INFO << "argus-tunnel: "
               << consumerReadyLine(nats_subject::kNotificationPushDelivery,
                                    nats_subject::kTunnelPushConsumer);
      break;
    case ConsumerEdge::Lost:
      LOG_WARN << "argus-tunnel: "
               << consumerLostLine(nats_subject::kNotificationPushDelivery,
                                   nats_subject::kTunnelPushConsumer);
      break;
    case ConsumerEdge::None:
      break;
  }
}

void SealedPushBridge::unsubscribe()
{
  if (bus_ && consumer_)
    bus_->unsubscribe(*consumer_);
  consumer_.reset();
}

void SealedPushBridge::onDelivery(const NatsBus::DurableMessage& message,
                                  NatsBus::DurableSettlement settlement)
{
  auto delivery = sealed_push_json::deliveryFromJson(std::string(message.payload));
  if (!delivery) {
    LOG_WARN << "argus-tunnel: protocol_error: push delivery is not well formed; dropped";
    settlement.term();
    return;
  }
  loop_.post([this, delivery = std::move(*delivery), settlement = std::move(settlement)]() mutable {
    static_cast<void>(dispatcher_->dispatch(delivery, std::move(settlement)));
  });
}

void SealedPushBridge::onPushResult(const PushResultEnvelope& result)
{
  dispatcher_->result(result);
}
}
