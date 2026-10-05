#pragma once

#include <shared/repositories/pending-object-delete/pending-object-delete-repository.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <trantor/net/EventLoop.h>

struct ObjectDeletionBackoffInput
{
  int64_t attempts{0};
  int64_t now{0};
};

namespace object_deletion
{
inline constexpr int64_t kBaseBackoffSeconds = 30;
inline constexpr int64_t kMaxBackoffSeconds = int64_t{6} * 3600;
inline constexpr int64_t kBatch = 50;

[[nodiscard]] int64_t nextAttemptAt(const ObjectDeletionBackoffInput& input);
}

class ObjectDeletionWorker
{
public:
  ObjectDeletionWorker() = default;
  ~ObjectDeletionWorker();

  ObjectDeletionWorker(const ObjectDeletionWorker&) = delete;
  ObjectDeletionWorker& operator=(const ObjectDeletionWorker&) = delete;

  static ObjectDeletionWorker& instance();

  void start();
  void stop();
  void kick();

  drogon::Task<std::size_t> drain(int64_t now);

private:
  void launch();

  PendingObjectDeleteRepository repository_;
  std::optional<trantor::TimerId> timer_;
  std::atomic<bool> running_{false};
  std::atomic<bool> again_{false};
};
