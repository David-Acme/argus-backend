#pragma once

#include <protocol/frames.hxx>

#include <atomic>
#include <deque>
#include <string>
#include <utility>

namespace tunnel
{
// Bounded push-intent queue with drop accounting; reject-new, nothing persists.
class PushQueue
{
public:
  explicit PushQueue(size_t capacity) : capacity_(capacity) {}

  bool push(std::string payload)
  {
    received_.fetch_add(1, std::memory_order_relaxed);
    if (payload.empty() || payload.size() > kMaxPayload ||
        size_.load(std::memory_order_relaxed) >= capacity_) {
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
