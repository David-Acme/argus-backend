#include "confirmation.hxx"

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

namespace argus::mcp
{

namespace
{
constexpr std::size_t kTokenLength = 6;
constexpr std::string_view kDigits = "0123456789abcdef";
}

bool ConfirmationKey::operator<(const ConfirmationKey& other) const
{
  return std::tie(userId, tool, target) < std::tie(other.userId, other.tool, other.target);
}

ConfirmationLedger::ConfirmationLedger(ConfirmationLimits limits, Clock clock)
    : limits_(limits), clock_(std::move(clock)), random_(std::random_device{}())
{
}

std::chrono::steady_clock::time_point ConfirmationLedger::now() const
{
  return clock_ ? clock_() : std::chrono::steady_clock::now();
}

void ConfirmationLedger::sweep(std::chrono::steady_clock::time_point now)
{
  std::erase_if(entries_, [now](const auto& entry) { return entry.second.expiresAt <= now; });
  while (entries_.size() >= limits_.capacity && !entries_.empty()) {
    const auto oldest = std::ranges::min_element(entries_, [](const auto& left, const auto& right) {
      return left.second.expiresAt < right.second.expiresAt;
    });
    entries_.erase(oldest);
  }
}

std::string ConfirmationLedger::issue(const ConfirmationKey& key)
{
  const std::scoped_lock lock(mutex_);
  const auto at = now();
  sweep(at);
  std::string token;
  for (std::size_t index = 0; index < kTokenLength; ++index)
    token.push_back(kDigits[random_() % kDigits.size()]);
  entries_.insert_or_assign(key, Entry{.token = token, .expiresAt = at + limits_.lifetime});
  return token;
}

bool ConfirmationLedger::consume(const ConfirmationKey& key, std::string_view token)
{
  const std::scoped_lock lock(mutex_);
  const auto found = entries_.find(key);
  if (found == entries_.end())
    return false;
  const bool live = found->second.expiresAt > now();
  const bool matches = found->second.token == token;
  if (live && matches) {
    entries_.erase(found);
    return true;
  }
  if (!live)
    entries_.erase(found);
  return false;
}

std::size_t ConfirmationLedger::pending() const
{
  const std::scoped_lock lock(mutex_);
  return entries_.size();
}

}
