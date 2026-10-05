#pragma once

#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>

class InferenceSlots
{
public:
  using Resume = std::function<void(std::coroutine_handle<>)>;

  explicit InferenceSlots(Resume resume);

  void open(std::size_t count);
  void acquire();
  [[nodiscard]] bool tryAcquire();
  void release();
  [[nodiscard]] std::size_t available() const;
  [[nodiscard]] std::size_t waiting() const;

  class Awaiter
  {
  public:
    explicit Awaiter(InferenceSlots& slots) : slots_(slots) {}
    [[nodiscard]] bool await_ready() { return slots_.tryAcquire(); }
    bool await_suspend(std::coroutine_handle<> handle);
    void await_resume() const noexcept {}

  private:
    InferenceSlots& slots_;
  };

  [[nodiscard]] Awaiter acquireAsync() { return Awaiter(*this); }

  class Permit
  {
  public:
    Permit() = default;
    explicit Permit(InferenceSlots& slots) : slots_(&slots) {}
    Permit(const Permit&) = delete;
    Permit& operator=(const Permit&) = delete;
    Permit(Permit&& other) noexcept : slots_(other.slots_) { other.slots_ = nullptr; }
    Permit& operator=(Permit&& other) noexcept;
    ~Permit();

  private:
    InferenceSlots* slots_{nullptr};
  };

private:
  Resume resume_;
  mutable std::mutex mutex_;
  std::condition_variable available_;
  std::size_t free_{0};
  std::size_t blockedWaiters_{0};
  std::deque<std::coroutine_handle<>> asyncWaiters_;
};
