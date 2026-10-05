#pragma once

#include "blocking-pool.hxx"

#include <coroutine>
#include <cstddef>
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
  explicit Dispatch(BlockingLane lane,
                    BlockingAdmission admission = BlockingAdmission::Queue)
      : lane_(lane), admission_(admission)
  {
  }

  explicit Dispatch(BlockingStrand& strand,
                    BlockingAdmission admission = BlockingAdmission::Queue)
      : strand_(&strand), admission_(admission)
  {
  }

  void operator()(std::function<void()> job) const
  {
    if (admission_ == BlockingAdmission::Queue) {
      if (strand_)
        strand_->post(std::move(job));
      else
        blocking_pool::submit(lane_, std::move(job));
      return;
    }
    const bool accepted = strand_ ? strand_->tryPost(std::move(job))
                                  : blocking_pool::trySubmit(lane_, std::move(job));
    if (!accepted)
      throw BlockingLaneFull("the blocking lane queue is full");
  }

private:
  BlockingLane lane_{BlockingLane::Light};
  BlockingStrand* strand_{nullptr};
  BlockingAdmission admission_{BlockingAdmission::Queue};
};

inline bool isAppLoop(const trantor::EventLoop* loop)
{
  auto& app = drogon::app();
  if (loop == app.getLoop())
    return true;
  if (!app.isRunning())
    return false;
  const std::size_t count = app.getThreadNum();
  for (std::size_t index = 0; index < count; ++index) {
    if (app.getIOLoop(index) == loop)
      return true;
  }
  return false;
}

inline trantor::EventLoop* resumeLoopFor(trantor::EventLoop* origin)
{
  if (origin != nullptr && isAppLoop(origin))
    return origin;
  return drogon::app().getLoop();
}

inline void resumeOn(trantor::EventLoop* loop, std::coroutine_handle<> handle)
{
  if (!loop->isRunning()) {
    handle.resume();
    return;
  }
  loop->queueInLoop([handle]() { handle.resume(); });
}

}

template <typename T>
class BlockingTask
{
public:
  explicit BlockingTask(std::function<T()> fn,
                        BlockingLane lane = BlockingLane::Light,
                        BlockingAdmission admission = BlockingAdmission::Queue)
      : fn_(std::move(fn)), dispatch_(lane, admission)
  {
  }

  BlockingTask(std::function<T()> fn, BlockingStrand& strand,
               BlockingAdmission admission = BlockingAdmission::Queue)
      : fn_(std::move(fn)), dispatch_(strand, admission)
  {
  }

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle)
  {
    state_ = std::make_shared<State>();
    trantor::EventLoop* loop = blocking_task::resumeLoopFor(
        trantor::EventLoop::getEventLoopOfCurrentThread());
    dispatch_([state = state_, fn = std::move(fn_), handle, loop]() mutable {
      try {
        state->value.emplace(fn());
      }
      catch (...) {
        state->exception = std::current_exception();
      }
      blocking_task::resumeOn(loop, handle);
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
                        BlockingLane lane = BlockingLane::Light,
                        BlockingAdmission admission = BlockingAdmission::Queue)
      : fn_(std::move(fn)), dispatch_(lane, admission)
  {
  }

  BlockingTask(std::function<void()> fn, BlockingStrand& strand,
               BlockingAdmission admission = BlockingAdmission::Queue)
      : fn_(std::move(fn)), dispatch_(strand, admission)
  {
  }

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle)
  {
    state_ = std::make_shared<State>();
    trantor::EventLoop* loop = blocking_task::resumeLoopFor(
        trantor::EventLoop::getEventLoopOfCurrentThread());
    dispatch_([state = state_, fn = std::move(fn_), handle, loop]() mutable {
      try {
        fn();
      }
      catch (...) {
        state->exception = std::current_exception();
      }
      blocking_task::resumeOn(loop, handle);
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
