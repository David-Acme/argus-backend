#pragma once

#include <algorithm>
#include <atomic>

class StreamSlots
{
public:
  explicit StreamSlots(int capacity) : capacity_(std::max(1, capacity)) {}

  [[nodiscard]] bool tryAcquire()
  {
    if (stopping_.load())
      return false;
    int current = active_.load();
    while (current < capacity_) {
      if (active_.compare_exchange_weak(current, current + 1))
        return true;
    }
    return false;
  }

  void release() { active_.fetch_sub(1); }

  void requestStop() { stopping_.store(true); }

  [[nodiscard]] bool stopping() const { return stopping_.load(); }

  [[nodiscard]] bool drained() const { return active_.load() == 0; }

private:
  int capacity_;
  std::atomic<int> active_{0};
  std::atomic<bool> stopping_{false};
};

class StreamLease
{
public:
  explicit StreamLease(StreamSlots& slots) : slots_(&slots) {}
  ~StreamLease() { release(); }

  void release()
  {
    if (slots_)
      slots_->release();
    slots_ = nullptr;
  }
  StreamLease(const StreamLease&) = delete;
  StreamLease& operator=(const StreamLease&) = delete;
  StreamLease(StreamLease&&) = delete;
  StreamLease& operator=(StreamLease&&) = delete;

private:
  StreamSlots* slots_;
};
