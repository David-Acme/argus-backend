#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

enum class PtzHolder : unsigned char
{
  None = 0,
  Argus,
  Viewer,
  Person
};

struct PtzLeaseConfig
{
  int64_t parkMs{60000};
  int64_t idleReleaseMs{60000};
};

struct PtzClaim
{
  int64_t cameraId{0};
  PtzHolder holder{PtzHolder::None};
  int64_t userId{0};
  int64_t atMs{0};
};

struct PtzLeaseState
{
  PtzHolder holder{PtzHolder::None};
  int64_t userId{0};
  int64_t sinceMs{0};
  int64_t parkUntilMs{0};
};

enum class PtzClaimResult : unsigned char
{
  Granted,
  Preempted,
  Refused
};

class PtzLease
{
public:
  static PtzLease& instance();

  void configure(PtzLeaseConfig config);
  [[nodiscard]] PtzClaimResult claim(const PtzClaim& claim);
  void release(const PtzClaim& claim);
  [[nodiscard]] PtzLeaseState stateOf(int64_t cameraId, int64_t atMs);
  [[nodiscard]] bool holds(const PtzClaim& claim);
  void forget(int64_t cameraId);

private:
  struct Entry
  {
    PtzHolder holder{PtzHolder::None};
    int64_t userId{0};
    int64_t sinceMs{0};
    int64_t lastActiveMs{0};
    int64_t parkUntilMs{0};
  };

  void expire(Entry& entry, int64_t atMs) const;
  void vacate(Entry& entry, int64_t atMs) const;

  mutable std::mutex mutex_;
  PtzLeaseConfig config_;
  std::unordered_map<int64_t, Entry> entries_;
};
