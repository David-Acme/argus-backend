#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

class WakeSignal
{
public:
  void notify()
  {
    {
      std::lock_guard lock(mutex_);
      pending_ = true;
    }
    ready_.notify_all();
  }

  void waitFor(std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(mutex_);
    ready_.wait_for(lock, timeout, [this] { return pending_; });
    pending_ = false;
  }

private:
  std::mutex mutex_;
  std::condition_variable ready_;
  bool pending_{false};
};
