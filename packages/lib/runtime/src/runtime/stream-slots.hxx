#pragma once

#include <algorithm>
#include <atomic>
#include <optional>
#include <utility>

class StreamSlots;

class StreamLease
{
public:
  StreamLease(StreamLease&& other) noexcept : slots_(std::exchange(other.slots_, nullptr)) {}

  StreamLease& operator=(StreamLease&& other) noexcept
  {
    if (this != &other) {
      release();
      slots_ = std::exchange(other.slots_, nullptr);
    }
    return *this;
  }

  StreamLease(const StreamLease&) = delete;
  StreamLease& operator=(const StreamLease&) = delete;

  ~StreamLease() { release(); }

  void release();

  [[nodiscard]] bool stopping() const;

private:
  friend class StreamSlots;

  explicit StreamLease(StreamSlots& slots) : slots_(&slots) {}

  StreamSlots* slots_;
};

class StreamSlots
{
public:
  explicit StreamSlots(int capacity) : capacity_(std::max(1, capacity)) {}

  StreamSlots(const StreamSlots&) = delete;
  StreamSlots& operator=(const StreamSlots&) = delete;
  StreamSlots(StreamSlots&&) = delete;
  StreamSlots& operator=(StreamSlots&&) = delete;
  ~StreamSlots() = default;

  [[nodiscard]] std::optional<StreamLease> tryAcquire()
  {
    if (stopping_.load())
      return std::nullopt;
    int current = active_.load();
    while (current < capacity_) {
      if (active_.compare_exchange_weak(current, current + 1))
        return StreamLease(*this);
    }
    return std::nullopt;
  }

  void requestStop() { stopping_.store(true); }

  [[nodiscard]] bool stopping() const { return stopping_.load(); }

  [[nodiscard]] bool drained() const { return active_.load() == 0; }

  [[nodiscard]] int active() const { return active_.load(); }

  [[nodiscard]] int capacity() const { return capacity_; }

private:
  friend class StreamLease;

  void release() { active_.fetch_sub(1); }

  int capacity_;
  std::atomic<int> active_{0};
  std::atomic<bool> stopping_{false};
};

inline void StreamLease::release()
{
  if (slots_ != nullptr)
    std::exchange(slots_, nullptr)->release();
}

inline bool StreamLease::stopping() const
{
  return slots_ == nullptr || slots_->stopping();
}
