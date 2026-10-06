#include "vision-component-host.hxx"

#include <stdexcept>
#include <string>
#include <utility>

VisionComponentHost::VisionComponentHost(VisionComponentHostInput input)
    : input_(std::move(input)),
      disk_({.modelsDir = input_.modelsDir,
             .owned = {std::string(kComponent)},
             .fetch = input_.fetch,
             .ready = [this](const std::string&) { return readyOrLoad(); }})
{
  if (!input_.loaded || !input_.load || !input_.unload)
    throw std::invalid_argument("The vision component host needs the engine's load, unload and state");
}

VisionComponentHost::~VisionComponentHost()
{
  {
    const std::scoped_lock lock(mutex_);
    state_ = LoadState::Removing;
  }
  if (loader_.joinable())
    loader_.join();
}

bool VisionComponentHost::readyOrLoad()
{
  if (input_.loaded())
    return true;
  const std::scoped_lock lock(mutex_);
  if (state_ != LoadState::Idle)
    return false;
  state_ = LoadState::Loading;
  if (loader_.joinable())
    loader_.join();
  loader_ = std::jthread([this] {
    input_.load();
    const std::scoped_lock done(mutex_);
    if (state_ == LoadState::Loading)
      state_ = LoadState::Tried;
  });
  return false;
}

ComponentStatus VisionComponentHost::status(const ComponentSpec& spec) const
{
  return disk_.status(spec);
}

ComponentStatus VisionComponentHost::install(const ComponentSpec& spec)
{
  {
    const std::scoped_lock lock(mutex_);
    if (state_ == LoadState::Tried)
      state_ = LoadState::Idle;
  }
  return disk_.install(spec);
}

ComponentStatus VisionComponentHost::cancel(const ComponentSpec& spec)
{
  return disk_.cancel(spec);
}

ComponentStatus VisionComponentHost::remove(const ComponentSpec& spec)
{
  if (spec.id != kComponent || !DiskComponentHost::acceptable(spec))
    throw std::invalid_argument("The component is not this owner's or is malformed");
  std::jthread loader;
  {
    const std::scoped_lock lock(mutex_);
    state_ = LoadState::Removing;
    loader = std::move(loader_);
  }
  if (loader.joinable())
    loader.join();
  input_.unload();
  ComponentStatus result;
  try {
    result = disk_.remove(spec);
  }
  catch (...) {
    const std::scoped_lock lock(mutex_);
    state_ = LoadState::Idle;
    throw;
  }
  const std::scoped_lock lock(mutex_);
  state_ = LoadState::Idle;
  return result;
}
