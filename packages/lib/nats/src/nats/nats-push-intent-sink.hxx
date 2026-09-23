#pragma once

#include <memory>
#include <nats/push-intent-sink.hxx>
#include <config/config-service.hxx>
#include <text/json-util.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

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
inline bool enabledFromConfig()
{
  return ConfigService::getBool("push.enabled");
}
}
