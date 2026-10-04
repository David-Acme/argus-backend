#include "call-feed.hxx"

#include <drogon/drogon.h>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <atomic>
#include <string>
#include <utility>

namespace
{
constexpr double kSweepPeriodS = 1.0;

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

void call_feed::subscribe(NatsBus& bus,
                          const std::shared_ptr<const CallEngine>& engine)
{
  bus.subscribe(nats_subject::kGuardKnownSeen,
                [engine](std::string_view, std::string_view payload) {
                  const auto event =
                      knownSeenFrom(json_util::fromString(std::string(payload)));
                  if (!event)
                    return;
                  drogon::app().getLoop()->queueInLoop([engine, event]() {
                    drogon::async_run([engine, event]() -> drogon::Task<void> {
                      try {
                        co_await engine->arrival(*event);
                      }
                      catch (const std::exception& error) {
                        LOG_WARN << "Call engine: arrival of person "
                                 << event->personId << " failed: "
                                 << error.what();
                      }
                    });
                  });
                });
}

void call_feed::startSweep(const std::shared_ptr<const CallEngine>& engine)
{
  auto running = std::make_shared<std::atomic<bool>>(false);
  drogon::app().getLoop()->runEvery(kSweepPeriodS, [engine, running]() {
    if (running->exchange(true))
      return;
    drogon::async_run([engine, running]() -> drogon::Task<void> {
      try {
        co_await engine->sweep();
      }
      catch (const std::exception& error) {
        LOG_WARN << "Call engine: sweep failed: " << error.what();
      }
      running->store(false);
    });
  });
}
