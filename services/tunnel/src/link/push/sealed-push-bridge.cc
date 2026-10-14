#include <link/push/sealed-push-bridge.hxx>

#include <drogon/drogon.h>
#include <json/value.h>
#include <nats/nats-subject.hxx>
#include <text/base64.hxx>
#include <text/json-util.hxx>

#include <utility>

namespace tunnel::link
{
namespace
{
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
}

namespace sealed_push_json
{
std::optional<PushSealedEnvelope> deliveryFromJson(const std::string& text)
{
  const Json::Value json = json_util::fromString(text);
  if (!json.isObject())
    return std::nullopt;
  const auto provider = textField(json, "provider");
  const auto kind = textField(json, "kind");
  const auto priority = textField(json, "priority");
  const auto ttl = ttlField(json);
  const auto deliveryId = bytesField(json, "deliveryId");
  const auto token = textField(json, "token");
  const auto collapseId = textField(json, "collapseId");
  const auto enc = bytesField(json, "enc");
  const auto ciphertext = bytesField(json, "ciphertext");
  if (!provider || !kind || !priority || !ttl || !deliveryId || !token ||
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
  return envelope;
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

void SealedPushLedger::remember(const std::string& deliveryId, const std::string& token)
{
  if (tokens_.contains(deliveryId))
    return;
  while (order_.size() >= capacity_) {
    tokens_.erase(order_.front());
    order_.pop_front();
  }
  order_.push_back(deliveryId);
  tokens_.emplace(deliveryId, token);
}

std::string SealedPushLedger::take(const std::string& deliveryId)
{
  const auto found = tokens_.find(deliveryId);
  if (found == tokens_.end())
    return {};
  std::string token = std::move(found->second);
  tokens_.erase(found);
  return token;
}

SealedPushBridge::SealedPushBridge(PollLoop& loop, std::shared_ptr<NatsBus> bus)
    : loop_(loop), bus_(std::move(bus))
{
}

void SealedPushBridge::attach(LinkClient& client)
{
  client_ = &client;
}

bool SealedPushBridge::subscribe()
{
  if (!bus_ || subscription_)
    return false;
  subscription_ = bus_->subscribe(nats_subject::kNotificationPushDelivery,
                                  [this](std::string_view, std::string_view payload) {
                                    onDelivery(payload);
                                  });
  return subscription_.has_value();
}

void SealedPushBridge::unsubscribe()
{
  if (bus_ && subscription_)
    bus_->unsubscribe(*subscription_);
  subscription_.reset();
}

void SealedPushBridge::onDelivery(std::string_view payload)
{
  const auto envelope = sealed_push_json::deliveryFromJson(std::string(payload));
  if (!envelope) {
    LOG_WARN << "argus-tunnel: protocol_error: push delivery is not well formed; dropped";
    return;
  }
  loop_.post([this, delivery = *envelope] { sendOnLoop(delivery); });
}

void SealedPushBridge::sendOnLoop(const PushSealedEnvelope& envelope)
{
  if (!client_)
    return;
  if (!client_->sendSealedPush(envelope))
    return;
  ledger_.remember(envelope.deliveryId, envelope.token);
}

void SealedPushBridge::onPushResult(const PushResultEnvelope& result)
{
  const std::string token = ledger_.take(result.deliveryId);
  if (!bus_)
    return;
  bus_->publish(nats_subject::kNotificationPushResult,
                sealed_push_json::resultToJson(result, token));
}
}
