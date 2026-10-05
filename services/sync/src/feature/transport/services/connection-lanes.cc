#include "connection-lanes.hxx"

#include <algorithm>
#include <drogon/drogon.h>
#include <exception>
#include <trantor/utils/Logger.h>
#include <utility>

ConnectionLanes::ConnectionLanes(FrameLaneConfig config) : config_(config) {}

ConnectionLanes::~ConnectionLanes()
{
  requestStop();
}

void ConnectionLanes::setDrainStarter(DrainStarter starter)
{
  std::scoped_lock lock(mutex_);
  starter_ = std::move(starter);
}

std::shared_ptr<FrameLane> ConnectionLanes::open(const LaneOpenInput& input)
{
  std::scoped_lock lock(mutex_);
  const auto [found, inserted] = entries_.try_emplace(input.conn.get());
  if (inserted || !found->second.lane ||
      found->second.conn.lock() != input.conn)
    found->second = Entry{
        .conn = input.conn,
        .lane = std::make_shared<FrameLane>(
            config_, FrameLaneOwner{.userId = input.userId, .loop = input.loop})};
  return found->second.lane;
}

std::shared_ptr<FrameLane>
ConnectionLanes::find(const drogon::WebSocketConnectionPtr& conn) const
{
  std::scoped_lock lock(mutex_);
  const auto found = entries_.find(conn.get());
  return found == entries_.end() ? nullptr : found->second.lane;
}

void ConnectionLanes::close(const drogon::WebSocketConnectionPtr& conn)
{
  std::scoped_lock lock(mutex_);
  entries_.erase(conn.get());
}

std::size_t ConnectionLanes::revalidateUser(int64_t userId)
{
  std::vector<std::pair<drogon::WebSocketConnectionPtr, std::shared_ptr<FrameLane>>>
      targets;
  {
    std::scoped_lock lock(mutex_);
    for (const auto& [raw, entry] : entries_) {
      if (entry.lane->userId() != userId)
        continue;
      if (auto conn = entry.conn.lock())
        targets.emplace_back(std::move(conn), entry.lane);
    }
  }
  for (const auto& [conn, lane] : targets) {
    if (lane->admitRevalidation() == FrameAdmission::Start)
      startDrain(conn, lane);
  }
  return targets.size();
}

std::vector<int64_t> ConnectionLanes::connectedUsers() const
{
  std::vector<int64_t> users;
  {
    std::scoped_lock lock(mutex_);
    users.reserve(entries_.size());
    for (const auto& [raw, entry] : entries_)
      users.push_back(entry.lane->userId());
  }
  std::ranges::sort(users);
  const auto [first, last] = std::ranges::unique(users);
  users.erase(first, last);
  return users;
}

void ConnectionLanes::startRevalidation(LaneRevalidationConfig config)
{
  if (timer_ || config.intervalSeconds <= 0.0)
    return;
  directory_ = std::move(config.directory);
  timer_ = drogon::app().getLoop()->runEvery(config.intervalSeconds, [this]() {
    if (stopping_.load(std::memory_order_acquire) ||
        sweeping_.exchange(true, std::memory_order_acq_rel))
      return;
    drogon::async_run([this]() -> drogon::Task<> {
      co_await revalidateAll();
      sweeping_.store(false, std::memory_order_release);
    });
  });
}

void ConnectionLanes::pruneExpired()
{
  std::scoped_lock lock(mutex_);
  std::erase_if(entries_,
                [](const auto& item) { return item.second.conn.expired(); });
}

drogon::Task<void> ConnectionLanes::revalidateAll()
{
  pruneExpired();
  for (const int64_t userId : connectedUsers()) {
    if (stopping_.load(std::memory_order_acquire))
      co_return;
    if (directory_) {
      try {
        const auto found = co_await directory_->lookup(userId);
        if (found.status == DirectoryLookupStatus::Unavailable)
          continue;
      }
      catch (const std::exception& error) {
        LOG_WARN << "Sync: revalidating user " << userId
                 << " failed: " << error.what();
        continue;
      }
    }
    revalidateUser(userId);
  }
}

void ConnectionLanes::startDrain(const drogon::WebSocketConnectionPtr& conn,
                                 const std::shared_ptr<FrameLane>& lane) const
{
  DrainStarter starter;
  {
    std::scoped_lock lock(mutex_);
    starter = starter_;
  }
  if (starter) {
    starter(conn, lane);
    return;
  }
  while (lane->next().has_value())
    continue;
}

void ConnectionLanes::requestStop()
{
  stopping_.store(true, std::memory_order_release);
  if (!timer_)
    return;
  if (drogon::app().isRunning())
    drogon::app().getLoop()->invalidateTimer(*timer_);
  timer_.reset();
}

bool ConnectionLanes::drained() const
{
  if (sweeping_.load(std::memory_order_acquire))
    return false;
  std::scoped_lock lock(mutex_);
  return std::ranges::none_of(entries_, [](const auto& item) {
    return item.second.lane->draining();
  });
}
