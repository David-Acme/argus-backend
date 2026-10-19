#pragma once

#include <guard/guard-presence-client.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

struct DutyGateConfig
{
  std::shared_ptr<const GuardPresenceClient> client;
  int64_t ttlMs{15000};
};

struct DutyInstant
{
  int64_t userId{0};
  int64_t atMs{0};
};

struct DutyAnswer
{
  int64_t userId{0};
  bool onDuty{false};
  int64_t atMs{0};
};

class DutyGate
{
public:
  static DutyGate& instance();

  void configure(DutyGateConfig config);
  drogon::Task<bool> onDuty(int64_t userId);
  [[nodiscard]] std::optional<bool> cached(const DutyInstant& instant);
  void remember(const DutyAnswer& answer);

private:
  struct Entry
  {
    bool onDuty{false};
    int64_t expiresAtMs{0};
  };

  std::mutex mutex_;
  DutyGateConfig config_;
  std::unordered_map<int64_t, Entry> entries_;
};
