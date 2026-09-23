#pragma once

#include <atomic>
#include <cstdint>

namespace in_flight
{

class Guard
{
public:
  explicit Guard(std::atomic<int64_t>& counter) : counter_(counter)
  {
    counter_.fetch_add(1, std::memory_order_acq_rel);
  }

  ~Guard() { counter_.fetch_sub(1, std::memory_order_acq_rel); }

  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;

private:
  std::atomic<int64_t>& counter_;
};

}
