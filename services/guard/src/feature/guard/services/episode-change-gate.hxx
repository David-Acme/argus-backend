#pragma once

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <trantor/net/EventLoop.h>

class ResourceChangeOutbox;

class EpisodeChangeGate
{
public:
  struct Config
  {
    double publishSeconds{15};
    std::function<int64_t()> clock;
  };

  EpisodeChangeGate(const ResourceChangeOutbox& changes, Config config);

  [[nodiscard]] drogon::Task<void> touch(int64_t encounterId);
  void published(int64_t encounterId);
  [[nodiscard]] drogon::Task<void> flush();

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  [[nodiscard]] bool dueLocked(int64_t encounterId, int64_t at) const;
  [[nodiscard]] int64_t now() const;
  void schedule();

  const ResourceChangeOutbox* changes_{nullptr};
  Config config_;
  mutable std::mutex mutex_;
  std::map<int64_t, int64_t> publishedAt_;
  std::set<int64_t> dirty_;
  std::optional<trantor::TimerId> timer_;
  std::atomic<bool> stopped_{false};
  std::atomic<int> pending_{0};
};
