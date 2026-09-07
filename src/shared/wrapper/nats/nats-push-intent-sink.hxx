#pragma once

#include <memory>
#include <shared/contracts/push-intent-sink.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

// Shared push-intent publisher: publishes one JSON intent per notification row
// over `argus.notification.v1.push_intent`. Both argus-notification and the
// gateway install it at boot behind [push] enabled — the gateway installs it
// too because the F3-2 cutover moved notification row creation into the
// gateway process (Ruling CK keeps argus-notification the policy owner).
class NatsPushIntentSink : public push_intent::PushIntentSink
{
public:
  explicit NatsPushIntentSink(std::shared_ptr<NatsBus> bus)
      : bus_(std::move(bus))
  {
  }

  void publish(const PushIntent& intent) const override
  {
    const std::string payload =
        json_util::toString(push_intent::toJson(intent));
    if (!bus_->publish(nats_subject::kNotificationPushIntent, payload))
      LOG_WARN << "Push intent: publish failed for notification "
               << intent.notificationId;
  }

private:
  std::shared_ptr<NatsBus> bus_;
};

namespace push_intent
{
// Gate: [push] enabled, default false so the app movil contract stays frozen
// (Ruling CM — zero behavior change when the section is absent).
inline bool enabledFromConfig()
{
  return ConfigService::getBool("push.enabled");
}
} // namespace push_intent
