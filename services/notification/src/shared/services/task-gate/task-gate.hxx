#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

class TaskGate
{
public:
  class Ticket
  {
  public:
    Ticket() = default;

    Ticket(std::shared_ptr<TaskGate> gate, bool admitted)
        : gate_(std::move(gate)), admitted_(admitted)
    {
    }

    Ticket(Ticket&& other) noexcept = default;
    Ticket& operator=(Ticket&&) = delete;
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;

    ~Ticket()
    {
      if (gate_)
        gate_->active_.fetch_sub(1, std::memory_order_acq_rel);
    }

    explicit operator bool() const { return admitted_; }

  private:
    std::shared_ptr<TaskGate> gate_;
    bool admitted_{false};
  };

  [[nodiscard]] static Ticket enter(const std::shared_ptr<TaskGate>& gate)
  {
    if (!gate)
      return {nullptr, true};
    gate->active_.fetch_add(1, std::memory_order_acq_rel);
    if (gate->stopping_.load(std::memory_order_acquire)) {
      gate->active_.fetch_sub(1, std::memory_order_acq_rel);
      return {nullptr, false};
    }
    return {gate, true};
  }

  void requestStop() { stopping_.store(true, std::memory_order_release); }

  [[nodiscard]] bool stopping() const
  {
    return stopping_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool drained() const
  {
    return active_.load(std::memory_order_acquire) == 0;
  }

private:
  std::atomic<bool> stopping_{false};
  std::atomic<int64_t> active_{0};
};
