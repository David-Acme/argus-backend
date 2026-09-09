#pragma once

#include <drogon/drogon.h>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

template <typename T>
class BlockingTask
{
public:
  explicit BlockingTask(std::function<T()> fn) : fn_(std::move(fn)) {}

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle)
  {
    auto state = std::make_shared<State>();
    state_ = state;
    std::thread([state, fn = std::move(fn_), handle]() mutable {
      try {
        state->value = fn();
      }
      catch (...) {
        state->exception = std::current_exception();
      }
      drogon::app().getLoop()->queueInLoop(
          [state, handle]() { handle.resume(); });
    }).detach();
  }

  T await_resume()
  {
    if (state_->exception)
      std::rethrow_exception(state_->exception);
    return std::move(state_->value);
  }

private:
  struct State
  {
    T value{};
    std::exception_ptr exception;
  };

  std::function<T()> fn_;
  std::shared_ptr<State> state_;
};

template <>
class BlockingTask<void>
{
public:
  explicit BlockingTask(std::function<void()> fn) : fn_(std::move(fn)) {}

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle)
  {
    auto state = std::make_shared<State>();
    state_ = state;
    std::thread([state, fn = std::move(fn_), handle]() mutable {
      try {
        fn();
      }
      catch (...) {
        state->exception = std::current_exception();
      }
      drogon::app().getLoop()->queueInLoop(
          [state, handle]() { handle.resume(); });
    }).detach();
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
  std::shared_ptr<State> state_;
};
