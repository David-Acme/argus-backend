#include "inference-slots.hxx"

#include <utility>

InferenceSlots::InferenceSlots(Resume resume) : resume_(std::move(resume)) {}

void InferenceSlots::open(std::size_t count)
{
  for (std::size_t i = 0; i < count; ++i)
    release();
}

bool InferenceSlots::tryAcquire()
{
  const std::scoped_lock lock(mutex_);
  if (free_ == 0)
    return false;
  --free_;
  return true;
}

void InferenceSlots::acquire()
{
  std::unique_lock lock(mutex_);
  ++blockedWaiters_;
  available_.wait(lock, [this] { return free_ > 0; });
  --blockedWaiters_;
  --free_;
}

bool InferenceSlots::Awaiter::await_suspend(std::coroutine_handle<> handle)
{
  const std::scoped_lock lock(slots_.mutex_);
  if (slots_.free_ > 0) {
    --slots_.free_;
    return false;
  }
  slots_.asyncWaiters_.push_back(handle);
  return true;
}

void InferenceSlots::release()
{
  std::coroutine_handle<> handoff;
  {
    const std::scoped_lock lock(mutex_);
    if (blockedWaiters_ == 0 && !asyncWaiters_.empty()) {
      handoff = asyncWaiters_.front();
      asyncWaiters_.pop_front();
    }
    else {
      ++free_;
    }
  }
  if (handoff) {
    resume_(handoff);
    return;
  }
  available_.notify_one();
}

std::size_t InferenceSlots::available() const
{
  const std::scoped_lock lock(mutex_);
  return free_;
}

std::size_t InferenceSlots::waiting() const
{
  const std::scoped_lock lock(mutex_);
  return blockedWaiters_ + asyncWaiters_.size();
}

InferenceSlots::Permit& InferenceSlots::Permit::operator=(Permit&& other) noexcept
{
  if (this != &other) {
    if (slots_ != nullptr)
      slots_->release();
    slots_ = other.slots_;
    other.slots_ = nullptr;
  }
  return *this;
}

InferenceSlots::Permit::~Permit()
{
  if (slots_ != nullptr)
    slots_->release();
}
