#include "push-alert-writer.hxx"

#include <shared/services/notification/push-copy.hxx>
#include <shared/services/push-delivery/push-delivery-wire.hxx>
#include <text/json-util.hxx>

#include <config/config-service.hxx>
#include <trantor/utils/Logger.h>

#include <cstddef>
#include <string>
#include <utility>

namespace
{
constexpr uint32_t kAlertTtlSeconds = 3600;

PushPriority priorityOf(const std::string& urgency)
{
  return urgency == "critical" || urgency == "time_sensitive" ? PushPriority::High
                                                              : PushPriority::Normal;
}
}

bool pushAlertsEnabled()
{
  return ConfigService::getBool("push.enabled");
}

PushAlertWriter::PushAlertWriter(PushAlertWriterDependencies dependencies)
    : devices_(std::move(dependencies.devices)),
      outbox_(std::move(dependencies.outbox)),
      clock_(std::move(dependencies.clock))
{
}

drogon::Task<void> PushAlertWriter::write(NotificationAlertRound round) const
{
  if (!devices_ || !outbox_)
    co_return;
  for (std::size_t index = 0; index < round.inputs.size(); ++index) {
    const NotificationCreateInput& input = round.inputs[index];
    if (input.userId <= 0 || index >= round.notificationIds.size())
      continue;
    const int64_t now = clock_();
    const PushCopy copy = push_copy::render({.lang = push_copy::langOf(input.data),
                                             .urgency = push_copy::urgencyOf(input.data),
                                             .call = false});
    const auto devices = co_await devices_->devicesOf(input.userId, round.client);
    for (const PushDevice& device : devices) {
      if (!push_envelope::combinationAllowed(device.provider, PushKind::Alert))
        continue;
      const std::string deliveryId = randomBytes(push_envelope::kDeliveryIdSize);
      if (deliveryId.size() != push_envelope::kDeliveryIdSize)
        continue;
      Json::Value intent(Json::objectValue);
      intent["kind"] = "notification";
      intent["notificationId"] = static_cast<Json::Int64>(round.notificationIds[index]);
      intent["type"] = input.type;
      intent["title"] = copy.title;
      intent["body"] = copy.body;
      intent["deepLink"] = "argus://notification?notificationId=" + std::to_string(round.notificationIds[index]);
      intent["iat"] = static_cast<Json::Int64>(now);
      intent["exp"] = static_cast<Json::Int64>(now + kAlertTtlSeconds);
      const PushSealOutcome sealed = sealer_.seal({.deliveryId = deliveryId,
                                                   .provider = device.provider,
                                                   .kind = PushKind::Alert,
                                                   .priority = priorityOf(push_copy::urgencyOf(input.data)),
                                                   .ttlSeconds = kAlertTtlSeconds,
                                                   .collapseId = push_delivery_wire::hexOf(deliveryId),
                                                   .token = device.token,
                                                   .publicKey = device.publicKey,
                                                   .keyId = device.keyId,
                                                   .plaintext = json_util::toString(intent)});
      if (sealed.status != PushSealStatus::Sealed) {
        LOG_WARN << "Notification alert: sealed delivery refused ("
                 << std::string(pushSealStatusToString(sealed.status)) << "); not sent";
        continue;
      }
      co_await outbox_->record(push_delivery_wire::recordOf(
          {.envelope = sealed.envelope, .expiresAtMs = (now + kAlertTtlSeconds) * 1000},
          round.client));
    }
  }
  co_return;
}
