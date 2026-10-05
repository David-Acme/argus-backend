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

struct PresenceFill
{
  const std::vector<PresenceEntry>& entries;
  uint64_t readMark{0};
};

class PresenceBoard
{
public:
  bool apply(const PresenceEntry& entry);
  [[nodiscard]] uint64_t mark() const;
  [[nodiscard]] std::vector<int64_t> fill(const PresenceFill& input);
  [[nodiscard]] PresenceEntry of(int64_t userId) const;
  [[nodiscard]] std::vector<int64_t> armedUsers() const;

  void markGuardSeen(int64_t at);
  [[nodiscard]] int64_t guardSeenAt() const;

private:
  struct Slot
  {
    PresenceEntry entry;
    uint64_t appliedSeq{0};
  };

  mutable std::mutex mutex_;
  std::unordered_map<int64_t, Slot> entries_;
  uint64_t sequence_{0};
  std::atomic<int64_t> guardSeenAt_{0};
};
