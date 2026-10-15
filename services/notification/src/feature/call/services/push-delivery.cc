#include <feature/call/services/push-delivery.hxx>
#include <feature/call/services/call-push-routing.hxx>

#include <shared/services/notification/push-copy.hxx>
#include <text/json-util.hxx>

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace
{
constexpr std::size_t kDeliveryIdSize = push_envelope::kDeliveryIdSize;
constexpr std::size_t kIntentIdSize = 16;
constexpr std::size_t kMaxCollapseIdSize = push_envelope::kMaxCollapseIdSize;
constexpr uint32_t kCancelTtlSeconds = 60;

std::string hexOf(const std::string& bytes)
{
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const char byte : bytes) {
    const auto value = static_cast<unsigned char>(byte);
    out.push_back(kDigits[value >> 4U]);
    out.push_back(kDigits[value & 0x0FU]);
  }
  return out;
}

std::string intentUrgency(const std::string& urgency)
{
  if (urgency == "critical" || urgency == "time_sensitive")
    return urgency;
  return "normal";
}

uint32_t remainingSeconds(int64_t expiresAt, int64_t now)
{
  const int64_t remaining = std::max<int64_t>(1, expiresAt - now);
  return static_cast<uint32_t>(std::min<int64_t>(remaining, UINT32_MAX));
}
}

std::string callDeepLink(const std::string& callId)
{
  return "argus://call?callId=" + callId;
}

CallPushDelivery::CallPushDelivery(std::shared_ptr<const CallPushDeviceDirectory> devices,
                                   std::shared_ptr<const CallPushSink> sink,
                                   std::function<int64_t()> clock)
    : devices_(std::move(devices)), sink_(std::move(sink)), clock_(std::move(clock))
{
}

drogon::Task<int64_t> CallPushDelivery::ring(const CallSchema& call) const
{
  co_return co_await deliver(call, false);
}

drogon::Task<int64_t> CallPushDelivery::cancel(const CallSchema& call) const
{
  co_return co_await deliver(call, true);
}

drogon::Task<int64_t> CallPushDelivery::deliver(const CallSchema& call, bool cancelling) const
{
  if (!devices_ || !sink_)
    co_return 0;
  const int64_t now = clock_();
  const auto devices = co_await devices_->devicesOf(call.userId);
  if (devices.empty())
    co_return 0;

  const PushCopy copy = push_copy::render({.lang = call.lang,
                                           .urgency = call.urgency,
                                           .call = true});
  Json::Value intent(Json::objectValue);
  intent["kind"] = "call";
  intent["callId"] = call.callId();
  intent["urgency"] = intentUrgency(call.urgency);
  intent["deepLink"] = callDeepLink(call.callId());
  intent["title"] = copy.title;
  intent["body"] = copy.body;
  intent["iat"] = Json::Int64(now);
  intent["exp"] = Json::Int64(call.expiresAt);
  const std::string intentId = randomBytes(kIntentIdSize);
  if (intentId.size() != kIntentIdSize)
    co_return 0;
  intent["intentId"] = hexOf(intentId);
  if (cancelling)
    intent["cancel"] = true;
  const std::string plaintext = json_util::toString(intent);
  const uint32_t ttl = cancelling ? kCancelTtlSeconds : remainingSeconds(call.expiresAt, now);
  const int64_t expiresAtMs = (cancelling ? now + kCancelTtlSeconds : call.expiresAt) * 1000;

  int64_t published = 0;
  for (const auto& device : call_push_routing::devicesForCall(devices)) {
    const std::string deliveryId = randomBytes(kDeliveryIdSize);
    if (deliveryId.size() != kDeliveryIdSize)
      continue;
    const PushSealOutcome sealed = sealer_.seal({.deliveryId = deliveryId,
                                                 .provider = device.provider,
                                                 .kind = PushKind::Call,
                                                 .priority = PushPriority::High,
                                                 .ttlSeconds = ttl,
                                                 .collapseId = call.callId().substr(
                                                     0, std::min(kMaxCollapseIdSize, call.callId().size())),
                                                 .token = device.token,
                                                 .publicKey = device.publicKey,
                                                 .keyId = device.keyId,
                                                 .plaintext = plaintext});
    if (sealed.status != PushSealStatus::Sealed) {
      LOG_WARN << "Call push: sealed delivery refused (" << std::string(pushSealStatusToString(sealed.status))
               << "); not sent";
      continue;
    }
    if (sink_->publish({.envelope = sealed.envelope, .expiresAtMs = expiresAtMs}))
      ++published;
  }
  co_return published;
}
