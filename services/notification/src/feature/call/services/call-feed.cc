#include "call-feed.hxx"

#include <drogon/drogon.h>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace
{
constexpr double kSweepPeriodS = 1.0;
constexpr int kSubscribeRetryBaseS = 5;
constexpr int kSubscribeRetryMaxS = 60;
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
           .quiet = true,
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

namespace
{
class KnownSeenRetry : public std::enable_shared_from_this<KnownSeenRetry>
{
public:
  explicit KnownSeenRetry(call_feed::FeedInput input)
      : input_(std::move(input))
  {
  }

  void start() { attempt(); }

private:
  void attempt()
  {
    if (input_.tasks && input_.tasks->stopping())
      return;
    const bool attached = input_.attempt ? input_.attempt() : trySubscribe(input_);
    if (attached) {
      LOG_INFO << "Call engine: durable " << call_feed::kKnownSeenDurable
               << " on " << nats_subject::kGuardKnownSeen;
      return;
    }
    const int failures = ++attempts_;
    if (failures == 1)
      LOG_WARN << "Call engine: " << nats_subject::kGuardKnownSeen
               << " durable unavailable; retrying with backoff up to "
               << kSubscribeRetryMaxS << " s";
    const int delay = input_.delay ? input_.delay(failures)
                                   : call_feed::retryDelaySeconds(failures);
    const std::shared_ptr<KnownSeenRetry> self = shared_from_this();
    drogon::app().getLoop()->runAfter(static_cast<double>(delay),
                                      [self]() { self->attempt(); });
  }

  call_feed::FeedInput input_;
  int attempts_{0};
};
}

void call_feed::subscribe(const FeedInput& input)
{
  if (!input.bus || !input.engine)
    return;
  std::make_shared<KnownSeenRetry>(input)->start();
}

int call_feed::retryDelaySeconds(int failures)
{
  const int step = std::clamp(failures - 1, 0, 16);
  const int64_t delay = static_cast<int64_t>(kSubscribeRetryBaseS) << step;
  return static_cast<int>(std::min<int64_t>(delay, kSubscribeRetryMaxS));
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
