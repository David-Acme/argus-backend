#include "heartbeat-feed.hxx"

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <nats/nats-bus.hxx>
#include <runtime/blocking-task.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <exception>
#include <utility>

namespace
{
int64_t nowSeconds()
{
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t nowMillis()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

std::optional<PresenceEntry> heartbeat::parsePresence(std::string_view payload)
{
  Json::Value json;
  try {
    json = json_util::fromString(std::string(payload));
  }
  catch (...) {
    return std::nullopt;
  }
  if (!json.isObject() || !json["userId"].isIntegral() ||
      !json["overall"].isString())
    return std::nullopt;
  const int64_t userId = json["userId"].asInt64();
  if (userId <= 0)
    return std::nullopt;
  const int64_t since =
      json["since"].isIntegral() ? json["since"].asInt64() : int64_t{0};
  return PresenceEntry{.userId = userId,
                       .overall = normalizePresence(json["overall"].asString()),
                       .since = since};
}

PushIntent heartbeat::pushIntentFor(int64_t userId, const Json::Value& heartbeat)
{
  Json::Value data = heartbeat;
  data["kind"] = "heartbeat";
  return PushIntent{.userId = userId,
                    .notificationId = 0,
                    .type = "heartbeat",
                    .title = {},
                    .body = {},
                    .createdAtMs = nowMillis(),
                    .data = std::move(data)};
}

HeartbeatFeed::HeartbeatFeed(Dependencies dependencies, Config config)
    : dependencies_(std::move(dependencies)),
      config_(std::move(config)),
      alive_(std::make_shared<bool>(true))
{
}

HeartbeatFeed::~HeartbeatFeed()
{
  stop();
}

void HeartbeatFeed::start()
{
  if (dependencies_.bus) {
    if (!config_.presenceSubject.empty()) {
      if (const auto id = dependencies_.bus->subscribe(
              config_.presenceSubject,
              [this](std::string_view, std::string_view payload) {
                ingestPresence(payload);
              }))
        subscriptions_.push_back(*id);
    }
    if (!config_.guardHeartbeatSubject.empty()) {
      if (const auto id = dependencies_.bus->subscribe(
              config_.guardHeartbeatSubject,
              [this](std::string_view, std::string_view) {
                ingestGuardHeartbeat(nowSeconds());
              }))
        subscriptions_.push_back(*id);
    }
  }
  auto* loop = drogon::app().getLoop();
  if (dependencies_.push && config_.pushIntervalSeconds > 0)
    pushTimer_ = loop->runEvery(config_.pushIntervalSeconds,
                                [this]() { static_cast<void>(publishPushHeartbeats()); });
  if (dependencies_.directory && config_.refillSeconds > 0) {
    const std::weak_ptr<bool> alive = alive_;
    const auto refilling = refilling_;
    const auto refillOffLoop = [this, alive, refilling]() {
      drogon::async_run([this, alive, refilling]() -> drogon::Task<> {
        if (alive.expired())
          co_return;
        refilling->fetch_add(1, std::memory_order_acq_rel);
        try {
          co_await BlockingTask<bool>{[this]() { return refill(); }};
        }
        catch (const std::exception& error) {
          LOG_WARN << "Heartbeat: presence refill failed: " << error.what();
        }
        refilling->fetch_sub(1, std::memory_order_acq_rel);
      });
    };
    loop->queueInLoop(refillOffLoop);
    refillTimer_ = loop->runEvery(config_.refillSeconds, refillOffLoop);
  }
}

void HeartbeatFeed::stop()
{
  if (dependencies_.bus) {
    for (const uint64_t id : subscriptions_)
      dependencies_.bus->unsubscribe(id);
  }
  subscriptions_.clear();
  if (pushTimer_)
    drogon::app().getLoop()->invalidateTimer(*pushTimer_);
  if (refillTimer_)
    drogon::app().getLoop()->invalidateTimer(*refillTimer_);
  pushTimer_.reset();
  refillTimer_.reset();
  alive_.reset();
}

void HeartbeatFeed::requestStop()
{
  stop();
}

bool HeartbeatFeed::drained() const
{
  return refilling_->load(std::memory_order_acquire) == 0;
}

bool HeartbeatFeed::ingestPresence(std::string_view payload)
{
  const auto entry = heartbeat::parsePresence(payload);
  if (!entry || !dependencies_.board)
    return false;
  if (!dependencies_.board->apply(*entry))
    return false;
  publishTo(entry->userId);
  return true;
}

void HeartbeatFeed::ingestGuardHeartbeat(int64_t at)
{
  if (dependencies_.board)
    dependencies_.board->markGuardSeen(at);
}

size_t HeartbeatFeed::publishPushHeartbeats() const
{
  if (!dependencies_.push || !dependencies_.board || !dependencies_.heartbeat)
    return 0;
  size_t published = 0;
  for (const int64_t userId : dependencies_.board->armedUsers()) {
    dependencies_.push->publish(heartbeat::pushIntentFor(
        userId, dependencies_.heartbeat->heartbeatFor(userId)));
    ++published;
  }
  return published;
}

bool HeartbeatFeed::refill()
{
  if (!dependencies_.directory || !dependencies_.board)
    return false;
  const uint64_t readMark = dependencies_.board->mark();
  const auto entries = dependencies_.directory->list();
  if (!entries) {
    LOG_WARN << "Heartbeat: presence directory unavailable; keeping the "
                "presence already known";
    return false;
  }
  for (const int64_t userId :
       dependencies_.board->fill({.entries = *entries, .readMark = readMark}))
    publishTo(userId);
  return true;
}

void HeartbeatFeed::publishTo(int64_t userId) const
{
  if (!dependencies_.emit || !dependencies_.heartbeat)
    return;
  dependencies_.emit(userId,
                     json_util::toString(dependencies_.heartbeat->frameFor(userId)));
}
