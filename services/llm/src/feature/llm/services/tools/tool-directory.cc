#include "tool-directory.hxx"

#include <algorithm>

ToolDirectory::ToolDirectory(ToolRegistry& registry, ToolDirectoryPace pace) : registry_(registry), pace_(pace) {}

ToolDirectory::~ToolDirectory()
{
  requestStop();
}

void ToolDirectory::start()
{
  if (worker_.joinable())
    return;
  running_.store(true, std::memory_order_release);
  worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void ToolDirectory::requestRefresh()
{
  {
    const std::scoped_lock lock(mutex_);
    refreshRequested_ = true;
  }
  wake_.notify_all();
}

void ToolDirectory::requestStop()
{
  worker_.request_stop();
  wake_.notify_all();
  if (worker_.joinable())
    worker_.join();
  running_.store(false, std::memory_order_release);
}

bool ToolDirectory::drained() const
{
  return !running_.load(std::memory_order_acquire);
}

void ToolDirectory::run(const std::stop_token& stop)
{
  auto retry = pace_.retryFloor;
  while (!stop.stop_requested()) {
    const auto outcome = registry_.refresh();
    auto wait = pace_.refreshEvery;
    if (outcome.failed.empty()) {
      retry = pace_.retryFloor;
    }
    else {
      wait = retry;
      retry = std::min(pace_.retryCeiling, retry * 2);
    }
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, stop, wait, [this] { return refreshRequested_; });
    refreshRequested_ = false;
  }
}
