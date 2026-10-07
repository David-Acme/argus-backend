#include "tapo-control-hub.hxx"

#include <mutex>
#include <unordered_map>

namespace
{
std::mutex gMutex;
std::unordered_map<std::string, tapo_control_hub::Hold> gHolds;
}

std::optional<tapo_control_hub::Hold> tapo_control_hub::holdFor(const std::string& endpoint,
                                                                 const int64_t nowMs)
{
  const std::scoped_lock guard(gMutex);
  const auto it = gHolds.find(endpoint);
  if (it == gHolds.end())
    return std::nullopt;
  if (it->second.untilMs <= nowMs) {
    gHolds.erase(it);
    return std::nullopt;
  }
  return it->second;
}

void tapo_control_hub::hold(const std::string& endpoint, const Hold& hold)
{
  const std::scoped_lock guard(gMutex);
  auto& current = gHolds[endpoint];
  if (current.untilMs < hold.untilMs)
    current = hold;
}

void tapo_control_hub::release(const std::string& endpoint)
{
  const std::scoped_lock guard(gMutex);
  gHolds.erase(endpoint);
}

void tapo_control_hub::reset()
{
  const std::scoped_lock guard(gMutex);
  gHolds.clear();
}
