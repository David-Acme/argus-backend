#include "push-alert-writer.hxx"

#include "push-alert-intent.hxx"

#include <shared/services/notification/push-copy.hxx>
#include <shared/services/push-delivery/push-delivery-wire.hxx>
#include <text/json-util.hxx>

#include <config/config-service.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace
{
constexpr uint32_t kAlertTtlSeconds = 3600;
constexpr std::size_t kIntentIdSize = 16;

PushPriority priorityOf(const std::string& urgency)
{
  return urgency == "critical" || urgency == "time_sensitive" ? PushPriority::High
                                                              : PushPriority::Normal;
}

std::string episodeOf(const Json::Value& data)
{
  if (!data.isObject() || !data.isMember("threadKey") || !data["threadKey"].isString())
    return {};
  return data["threadKey"].asString();
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
    const std::string episode = episodeOf(input.data);
    const bool ringFollows =
        !episode.empty() &&
        std::ranges::find(round.ringFollowingUserIds, input.userId) != round.ringFollowingUserIds.end();
    const auto devices = co_await devices_->devicesOf(input.userId, round.client);
    for (const PushDevice& device : devices) {
      if (!push_envelope::combinationAllowed(device.provider, PushKind::Alert))
        continue;
      const std::string deliveryId = randomBytes(push_envelope::kDeliveryIdSize);
      if (deliveryId.size() != push_envelope::kDeliveryIdSize)
        continue;
      const std::string intentIdBytes = randomBytes(kIntentIdSize);
      if (intentIdBytes.size() != kIntentIdSize)
        continue;
      const std::string urgency = push_copy::urgencyOf(input.data);
      const Json::Value intent = push_alert_intent::build({
          .intentId = push_delivery_wire::hexOf(intentIdBytes),
          .notificationId = round.notificationIds[index],
          .type = input.type,
          .title = copy.title,
          .body = copy.body,
          .urgency = urgency,
          .episode = episode,
          .ringFollows = ringFollows,
          .issuedAt = now,
          .expiresAt = now + kAlertTtlSeconds,
      });
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
      LOG_INFO << "Alert push: delivery " << push_delivery_wire::hexOf(deliveryId) << " queued (ringFollows="
               << (ringFollows ? "true" : "false") << ", episode=" << (episode.empty() ? "absent" : "present")
               << ")";
    }
  }
  co_return;
}
