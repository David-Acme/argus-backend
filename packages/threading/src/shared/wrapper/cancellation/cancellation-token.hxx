#pragma once

#include <atomic>
#include <memory>

class CancellationToken
{
public:
  CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

  void cancel() const
  {
    if (flag_)
      flag_->store(true, std::memory_order_relaxed);
  }

  void reset() const
  {
    if (flag_)
      flag_->store(false, std::memory_order_relaxed);
  }

  bool cancelled() const
  {
    return flag_ && flag_->load(std::memory_order_relaxed);
  }

private:
  std::shared_ptr<std::atomic<bool>> flag_;
};
