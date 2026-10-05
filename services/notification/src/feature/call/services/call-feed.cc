#include "call-feed.hxx"

#include <drogon/drogon.h>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <atomic>
#include <optional>
#include <string>
#include <utility>

namespace
{
constexpr double kSweepPeriodS = 1.0;
constexpr double kSubscribeRetryS = 5.0;
constexpr int kMaxDeliver = 5;

std::string text(const Json::Value& data, const char* key)
{
  const Json::Value& value = data[key];
  return value.isString() ? value.asString() : std::string{};
}

int64_t number(const Json::Value& data, const char* key)
{
  const Json::Value& value = data[key];
  return value.isIntegral() ? value.asInt64() : 0;
}

void settle(const std::function<void()>& action)
{
  if (action)
    action();
}

struct KnownSeenDelivery
{
  const call_feed::FeedInput& input;
  const NatsBus::DurableMessage& message;
  NatsBus::DurableSettlement settlement;
};

void handleKnownSeen(KnownSeenDelivery delivery)
{
  const auto event = call_feed::knownSeenFrom(
      json_util::fromString(std::string(delivery.message.payload)));
  if (!event) {
    settle(delivery.settlement.term);
    return;
  }
  drogon::app().getLoop()->queueInLoop(
      [engine = delivery.input.engine, tasks = delivery.input.tasks,
       event = *event, settlement = std::move(delivery.settlement)]() {
        drogon::async_run([engine, tasks, event,
                           settlement]() -> drogon::Task<void> {
          const auto ticket = TaskGate::enter(tasks);
          if (!ticket) {
            settle(settlement.nak);
            co_return;
          }
          bool handled = true;
          try {
            co_await engine->arrival(event);
          }
          catch (const std::exception& error) {
            handled = false;
            LOG_WARN << "Call engine: arrival of person " << event.personId
                     << " failed: " << error.what();
          }
          settle(handled ? settlement.ack : settlement.nak);
        });
      });
}

bool trySubscribe(const call_feed::FeedInput& input)
{
  return input.bus
      ->subscribeDurable(
          {.stream = nats_subject::kGuardStream,
           .durable = call_feed::kKnownSeenDurable,
           .subject = nats_subject::kGuardKnownSeen,
           .deliverAll = false,
           .maxDeliver = kMaxDeliver,
           .maxAckPending = NatsBus::kOrderedMaxAckPending,
           .handler =
               [input](const NatsBus::DurableMessage& message,
                       NatsBus::DurableSettlement settlement) {
                 handleKnownSeen({.input = input,
                                  .message = message,
                                  .settlement = std::move(settlement)});
               }})
      .has_value();
}
}

std::optional<KnownSeenEvent> call_feed::knownSeenFrom(const Json::Value& payload)
{
  if (!payload.isObject())
    return std::nullopt;
  KnownSeenEvent event{.personId = number(payload, "personId"),
                       .cameraId = number(payload, "cameraId"),
                       .cameraName = text(payload, "cameraName"),
                       .environmentId = number(payload, "environmentId"),
                       .environmentName = text(payload, "environmentName"),
                       .at = number(payload, "at")};
  if (event.personId <= 0 || event.at <= 0)
    return std::nullopt;
  return event;
}

void call_feed::subscribe(const FeedInput& input)
{
  if (!input.bus || !input.engine)
    return;
  if (trySubscribe(input)) {
    LOG_INFO << "Call engine: durable " << kKnownSeenDurable << " on "
             << nats_subject::kGuardKnownSeen;
    return;
  }
  LOG_WARN << "Call engine: " << nats_subject::kGuardKnownSeen
           << " durable unavailable; retrying every " << kSubscribeRetryS << " s";
  auto timer = std::make_shared<std::optional<trantor::TimerId>>();
  *timer = drogon::app().getLoop()->runEvery(
      kSubscribeRetryS, [input, timer]() {
        if (!timer->has_value() || (input.tasks && input.tasks->stopping()) ||
            !trySubscribe(input))
          return;
        drogon::app().getLoop()->invalidateTimer(**timer);
        timer->reset();
        LOG_INFO << "Call engine: durable " << kKnownSeenDurable << " on "
                 << nats_subject::kGuardKnownSeen;
      });
}

void call_feed::startSweep(const SweepInput& input)
{
  auto running = std::make_shared<std::atomic<bool>>(false);
  drogon::app().getLoop()->runEvery(
      kSweepPeriodS, [engine = input.engine, tasks = input.tasks, running]() {
        if (running->exchange(true))
          return;
        drogon::async_run([engine, tasks, running]() -> drogon::Task<void> {
          const auto ticket = TaskGate::enter(tasks);
          if (ticket) {
            try {
              co_await engine->sweep();
            }
            catch (const std::exception& error) {
              LOG_WARN << "Call engine: sweep failed: " << error.what();
            }
          }
          running->store(false);
        });
      });
}
