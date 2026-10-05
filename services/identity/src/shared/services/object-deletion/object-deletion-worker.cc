#include "object-deletion-worker.hxx"

#include <algorithm>
#include <ctime>
#include <drogon/drogon.h>
#include <exception>
#include <storage/s3-storage-service.hxx>

namespace
{
constexpr double kSweepIntervalSeconds = 60.0;
constexpr int64_t kMaxShift = 20;
}

int64_t object_deletion::nextAttemptAt(const ObjectDeletionBackoffInput& input)
{
  const int64_t shift = std::clamp<int64_t>(input.attempts, 0, kMaxShift);
  const int64_t delay = std::min(kBaseBackoffSeconds << shift, kMaxBackoffSeconds);
  return input.now + delay;
}

ObjectDeletionWorker::~ObjectDeletionWorker()
{
  stop();
}

ObjectDeletionWorker& ObjectDeletionWorker::instance()
{
  static ObjectDeletionWorker worker;
  return worker;
}

void ObjectDeletionWorker::start()
{
  timer_ = drogon::app().getLoop()->runEvery(kSweepIntervalSeconds,
                                             [this] { launch(); });
  launch();
}

void ObjectDeletionWorker::stop()
{
  if (timer_)
    drogon::app().getLoop()->invalidateTimer(*timer_);
  timer_.reset();
}

void ObjectDeletionWorker::kick()
{
  drogon::app().getLoop()->queueInLoop([this] { launch(); });
}

void ObjectDeletionWorker::launch()
{
  if (running_.exchange(true)) {
    again_.store(true);
    return;
  }
  drogon::async_run([this]() -> drogon::Task<void> {
    do {
      again_.store(false);
      try {
        const auto deleted = co_await drain(std::time(nullptr));
        if (deleted > 0)
          LOG_INFO << "Object deletion: removed " << deleted
                   << " private object(s) that were waiting for storage";
      }
      catch (const std::exception& error) {
        LOG_WARN << "Object deletion: pass failed: " << error.what();
      }
    } while (again_.load());
    running_.store(false);
  });
}

drogon::Task<std::size_t> ObjectDeletionWorker::drain(int64_t now)
{
  const S3StorageService storage;
  if (!storage.isConfigured())
    co_return 0;
  const auto due = co_await repository_.findDue(
      {.now = now, .limit = object_deletion::kBatch});
  std::size_t deleted = 0;
  for (const auto& pending : due) {
    bool removed = false;
    try {
      co_await storage.remove(pending.objectKey);
      removed = true;
    }
    catch (const std::exception& error) {
      LOG_WARN << "Object deletion: a private object is still waiting ("
               << pending.attempts + 1 << " attempt(s)): " << error.what();
    }
    if (removed) {
      co_await repository_.remove(pending.id);
      ++deleted;
      continue;
    }
    co_await repository_.postpone(
        {.id = pending.id,
         .nextAttemptAt = object_deletion::nextAttemptAt(
             {.attempts = pending.attempts, .now = now})});
  }
  co_return deleted;
}
