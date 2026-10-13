#include "remote-link-feed.hxx"

#include <nats/nats-bus.hxx>
#include <shared/services/context/user-context.hxx>

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <string>
#include <utility>

namespace
{
int64_t nowMillis()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

RemoteLinkFeed::RemoteLinkFeed(RemoteLinkFeedDependencies dependencies, RemoteLinkFeedConfig config)
    : dependencies_(dependencies),
      config_(std::move(config)),
      gate_(config_.gate),
      refusals_(config_.refusalLogWindowMs),
      alive_(std::make_shared<bool>(true))
{
}

RemoteLinkFeed::~RemoteLinkFeed()
{
  stop();
}

void RemoteLinkFeed::start()
{
  const std::weak_ptr<bool> alive = alive_;
  if (dependencies_.bus && !config_.subject.empty()) {
    subscription_ = dependencies_.bus->subscribe(
        config_.subject, [this, alive](std::string_view, std::string_view payload) {
          drogon::app().getLoop()->queueInLoop([this, alive, frame = std::string(payload)] {
            if (alive.expired())
              return;
            ingest(frame, nowMillis());
          });
        });
  }
  timer_ = drogon::app().getLoop()->runEvery(config_.tickSeconds, [this, alive] {
    if (alive.expired())
      return;
    tick(nowMillis());
  });
}

void RemoteLinkFeed::stop()
{
  if (stopped_.exchange(true))
    return;
  if (dependencies_.bus && subscription_)
    dependencies_.bus->unsubscribe(*subscription_);
  subscription_.reset();
  if (timer_)
    drogon::app().getLoop()->invalidateTimer(*timer_);
  timer_.reset();
  alive_.reset();
}

void RemoteLinkFeed::requestStop()
{
  stop();
}

bool RemoteLinkFeed::drained() const
{
  return stopped_.load();
}

void RemoteLinkFeed::ingest(std::string_view payload, int64_t nowMs)
{
  const routes::RemoteLinkRefusal refusal = gate_.ingest(payload, nowMs);
  if (refusal != routes::RemoteLinkRefusal::None) {
    if (const auto dropped = refusals_.record(nowMs))
      LOG_WARN << "Remote link: dropped " << *dropped << " frame(s) on " << config_.subject << " ("
               << routes::remoteLinkRefusalToString(refusal) << "); the last valid state stays";
    return;
  }
  userContext().setRemote(gate_.state());
  publishState(nowMs);
}

void RemoteLinkFeed::tick(int64_t nowMs)
{
  if (gate_.expire(nowMs)) {
    LOG_WARN << "Remote link: nothing heard on " << config_.subject << " for "
             << config_.gate.staleAfterMs / 1000 << " s; the link reads down, the home id stays";
    userContext().setRemote(gate_.state());
  }
  publishState(nowMs);
}

void RemoteLinkFeed::publishState(int64_t nowMs)
{
  if (!gate_.shouldSend(nowMs))
    return;
  gate_.markSent(nowMs);
  userContext().remoteChanged();
}
