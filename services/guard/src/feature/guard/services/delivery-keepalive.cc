#include "delivery-keepalive.hxx"

#include <utility>
#include <vector>

uint64_t DeliveryKeepalive::hold(std::function<void()> inProgress)
{
  if (!inProgress)
    return 0;
  std::scoped_lock lock(mutex_);
  const uint64_t token = ++next_;
  held_.emplace(token, std::move(inProgress));
  return token;
}

void DeliveryKeepalive::release(uint64_t token)
{
  if (token == 0)
    return;
  std::scoped_lock lock(mutex_);
  held_.erase(token);
}

size_t DeliveryKeepalive::touch() const
{
  std::vector<std::function<void()>> pending;
  {
    std::scoped_lock lock(mutex_);
    pending.reserve(held_.size());
    for (const auto& [token, inProgress] : held_)
      pending.push_back(inProgress);
  }
  for (const auto& inProgress : pending)
    inProgress();
  return pending.size();
}

size_t DeliveryKeepalive::held() const
{
  std::scoped_lock lock(mutex_);
  return held_.size();
}
