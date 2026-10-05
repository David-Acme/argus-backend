#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct PresenceEntry
{
  int64_t userId{0};
  std::string overall;
  int64_t since{0};
};

class PresenceBoard
{
public:
  bool apply(const PresenceEntry& entry);
  void fill(const std::vector<PresenceEntry>& entries);
  [[nodiscard]] PresenceEntry of(int64_t userId) const;
  [[nodiscard]] std::vector<int64_t> armedUsers() const;

  void markGuardSeen(int64_t at);
  [[nodiscard]] int64_t guardSeenAt() const;

private:
  mutable std::mutex mutex_;
  std::unordered_map<int64_t, PresenceEntry> entries_;
  std::atomic<int64_t> guardSeenAt_{0};
};
