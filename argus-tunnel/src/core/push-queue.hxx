#pragma once

#include <atomic>
#include <deque>
#include <string>
#include <utility>

namespace tunnel
{
// Bounded in-memory push-intent queue (F5-5): the relay buffers intents while
// the home link is down, the client buffers intents for the device. The deque
// is touched only on the owning PollLoop thread; the size/drop counters are
// atomic so /health can read them from the Drogon thread. Nothing persists:
// past the capacity the oldest accounting is a drop (Ruling CL — no database
// in the tunnel).
class PushQueue
{
public:
  explicit PushQueue(size_t capacity) : capacity_(capacity) {}

  bool push(std::string payload)
  {
    received_.fetch_add(1, std::memory_order_relaxed);
    if (size_.load(std::memory_order_relaxed) >= capacity_) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    queue_.push_back(std::move(payload));
    size_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  std::string pop()
  {
    if (queue_.empty())
      return {};
    std::string payload = std::move(queue_.front());
    queue_.pop_front();
    size_.fetch_sub(1, std::memory_order_relaxed);
    return payload;
  }

  bool empty() const { return size_.load(std::memory_order_relaxed) == 0; }
  size_t size() const { return size_.load(std::memory_order_relaxed); }
  uint64_t received() const { return received_.load(std::memory_order_relaxed); }
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
  std::deque<std::string> queue_;
  size_t capacity_;
  std::atomic<size_t> size_{0};
  std::atomic<uint64_t> received_{0};
  std::atomic<uint64_t> dropped_{0};
};
} // namespace tunnel
