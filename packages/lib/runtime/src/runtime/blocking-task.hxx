#pragma once

#include "blocking-pool.hxx"

#include <coroutine>
#include <drogon/drogon.h>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace blocking_task
{

class Dispatch
{
public:
  explicit Dispatch(BlockingLane lane) : lane_(lane) {}

  explicit Dispatch(BlockingStrand& strand) : strand_(&strand) {}

  void operator()(std::function<void()> job) const
  {
    if (strand_)
      strand_->post(std::move(job));
    else
      blocking_pool::submit(lane_, std::move(job));
  }

private:
  BlockingLane lane_{BlockingLane::Light};
  BlockingStrand* strand_{nullptr};
};

inline void resumeOnLoop(std::coroutine_handle<> handle)
{
  drogon::app().getLoop()->queueInLoop([handle]() { handle.resume(); });
}

}

template <typename T>
class BlockingTask
{
public:
  explicit BlockingTask(std::function<T()> fn,
                        BlockingLane lane = BlockingLane::Light)
      : fn_(std::move(fn)), dispatch_(lane)
  {
  }

  BlockingTask(std::function<T()> fn, BlockingStrand& strand)
      : fn_(std::move(fn)), dispatch_(strand)
  {
  }

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle)
  {
    state_ = std::make_shared<State>();
    dispatch_([state = state_, fn = std::move(fn_), handle]() mutable {
      try {
        state->value.emplace(fn());
      }
      catch (...) {
        state->exception = std::current_exception();
      }
      blocking_task::resumeOnLoop(handle);
    });
  }

  T await_resume()
  {
    if (state_->exception)
      std::rethrow_exception(state_->exception);
    if (!state_->value)
      throw std::logic_error("BlockingTask resumed without a value");
    return std::move(*state_->value);
  }

private:
  struct State
  {
    std::optional<T> value;
    std::exception_ptr exception;
  };

  std::function<T()> fn_;
  blocking_task::Dispatch dispatch_;
  std::shared_ptr<State> state_;
};

template <>
class BlockingTask<void>
{
public:
  explicit BlockingTask(std::function<void()> fn,
                        BlockingLane lane = BlockingLane::Light)
      : fn_(std::move(fn)), dispatch_(lane)
  {
  }

  BlockingTask(std::function<void()> fn, BlockingStrand& strand)
      : fn_(std::move(fn)), dispatch_(strand)
  {
  }

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle)
  {
    state_ = std::make_shared<State>();
    dispatch_([state = state_, fn = std::move(fn_), handle]() mutable {
      try {
        fn();
      }
      catch (...) {
        state->exception = std::current_exception();
      }
      blocking_task::resumeOnLoop(handle);
    });
  }

  void await_resume()
  {
    if (state_->exception)
      std::rethrow_exception(state_->exception);
  }

private:
  struct State
  {
    std::exception_ptr exception;
  };

  std::function<void()> fn_;
  blocking_task::Dispatch dispatch_;
  std::shared_ptr<State> state_;
};
