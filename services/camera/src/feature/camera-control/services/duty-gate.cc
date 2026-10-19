#include "duty-gate.hxx"

#include <runtime/blocking-task.hxx>

#include <chrono>
#include <utility>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

DutyGate& DutyGate::instance()
{
  static DutyGate gate;
  return gate;
}

void DutyGate::configure(DutyGateConfig config)
{
  const std::scoped_lock lock(mutex_);
  config_ = std::move(config);
  entries_.clear();
}

std::optional<bool> DutyGate::cached(const DutyInstant& instant)
{
  const std::scoped_lock lock(mutex_);
  const auto found = entries_.find(instant.userId);
  if (found == entries_.end() || found->second.expiresAtMs <= instant.atMs)
    return std::nullopt;
  return found->second.onDuty;
}

void DutyGate::remember(const DutyAnswer& answer)
{
  const std::scoped_lock lock(mutex_);
  entries_[answer.userId] = {.onDuty = answer.onDuty, .expiresAtMs = answer.atMs + config_.ttlMs};
}

drogon::Task<bool> DutyGate::onDuty(int64_t userId)
{
  if (const auto known = cached({.userId = userId, .atMs = nowMs()}))
    co_return *known;
  std::shared_ptr<const GuardPresenceClient> client;
  {
    const std::scoped_lock lock(mutex_);
    client = config_.client;
  }
  if (!client)
    co_return false;
  const std::optional<bool> answer =
      co_await BlockingTask<std::optional<bool>>([client, userId]() { return client->onDuty(userId); });
  if (!answer)
    co_return false;
  remember({.userId = userId, .onDuty = *answer, .atMs = nowMs()});
  co_return *answer;
}
