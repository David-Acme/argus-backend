#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared/repositories/job/job-repository.hxx>
#include <shared/services/queue/job-types.hxx>
#include <string>
#include <thread>
#include <vector>

class QueueManager
{
public:
  QueueManager();
  ~QueueManager();

  QueueManager(const QueueManager&) = delete;
  QueueManager& operator=(const QueueManager&) = delete;

  void registerQueue(const QueueConfig& config, JobHandler handler);
  int64_t add(const std::string& queue, const std::string& payload,
              const JobOptions& options = {});
  bool start(const std::string& dbPath);
  void drain(int timeoutMs);
  void shutdown();
  QueueStats stats(const std::string& queue) const;

private:
  class Queue;
  std::vector<std::unique_ptr<Queue>> queues_;
  JobRepository repository_;
  std::string dbPath_;
  bool started_ = false;
};
