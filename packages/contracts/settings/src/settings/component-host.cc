#include "component-host.hxx"

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace
{
constexpr std::size_t kShaLength = 64;
constexpr std::string_view kPartSuffix = ".part";
constexpr std::string_view kSidecarSuffix = ".part.json";
constexpr std::string_view kNetwork = "network";

bool safeRelative(const std::string& path)
{
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos)
    return false;
  const std::filesystem::path relative(path);
  return std::ranges::none_of(relative, [](const std::filesystem::path& part) {
    return part == ".." || part == "." || part.empty();
  });
}

bool hexSha(const std::string& sha)
{
  return sha.size() == kShaLength &&
         std::ranges::all_of(sha, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::int64_t sizeOf(const std::filesystem::path& path)
{
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error))
    return -1;
  const auto size = std::filesystem::file_size(path, error);
  return error ? -1 : static_cast<std::int64_t>(size);
}

std::filesystem::path withSuffix(const std::filesystem::path& target, std::string_view suffix)
{
  auto text = target.string();
  text += suffix;
  return text;
}

bool complete(const ComponentSpec& spec, const ComponentFile& file, std::int64_t size)
{
  if (spec.source == ComponentSource::Download)
    return size == file.sizeBytes;
  return size > 0;
}
}

DiskComponentHost::DiskComponentHost(DiskComponentHostInput input) : input_(std::move(input))
{
  if (input_.modelsDir.empty())
    throw std::invalid_argument("A component host needs a models directory");
}

DiskComponentHost::~DiskComponentHost()
{
  std::map<std::string, std::shared_ptr<Activity>> activities;
  {
    const std::scoped_lock lock(mutex_);
    activities.swap(activities_);
  }
  for (auto& [id, activity] : activities) {
    activity->worker.request_stop();
    if (activity->worker.joinable())
      activity->worker.join();
  }
}

bool DiskComponentHost::acceptable(const ComponentSpec& spec)
{
  if (spec.id.empty() || spec.files.empty())
    return false;
  return std::ranges::all_of(spec.files, [&spec](const ComponentFile& file) {
    if (!safeRelative(file.path) || file.sizeBytes <= 0)
      return false;
    if (spec.source != ComponentSource::Download)
      return true;
    return file.url.starts_with("https://") && hexSha(file.sha256);
  });
}

void DiskComponentHost::check(const ComponentSpec& spec) const
{
  if (!acceptable(spec) || std::ranges::find(input_.owned, spec.id) == input_.owned.end())
    throw std::invalid_argument("The component is not one of this owner's or is malformed");
}

ComponentStatus DiskComponentHost::status(const ComponentSpec& spec) const
{
  check(spec);
  const std::scoped_lock lock(mutex_);
  return statusLocked(spec);
}

ComponentStatus DiskComponentHost::statusLocked(const ComponentSpec& spec) const
{
  ComponentStatus status{.id = spec.id,
                         .state = ComponentState::Missing,
                         .bytesPresent = 0,
                         .bytesTotal = spec.totalBytes(),
                         .ready = false,
                         .hostCommand = spec.source == ComponentSource::Provisioned ? spec.hostCommand : std::string{},
                         .reason = {}};
  bool whole = true;
  for (const auto& file : spec.files) {
    const auto target = input_.modelsDir / file.path;
    const auto size = sizeOf(target);
    if (complete(spec, file, size)) {
      status.bytesPresent += file.sizeBytes;
      continue;
    }
    whole = false;
    const auto partial = sizeOf(withSuffix(target, kPartSuffix));
    if (partial > 0)
      status.bytesPresent += std::min(partial, file.sizeBytes);
  }
  if (whole) {
    status.state = ComponentState::Installed;
    status.ready = input_.ready ? input_.ready(spec.id) : true;
    return status;
  }
  const auto activity = activities_.find(spec.id);
  if (activity != activities_.end() && activity->second->running)
    status.state = ComponentState::Installing;
  else if (activity != activities_.end() && !activity->second->failure.empty()) {
    status.state = ComponentState::Failed;
    status.reason = activity->second->failure;
  }
  else if (spec.source == ComponentSource::Provisioned)
    status.state = ComponentState::HostOnly;
  return status;
}

ComponentStatus DiskComponentHost::install(const ComponentSpec& spec)
{
  check(spec);
  std::shared_ptr<Activity> finished;
  ComponentStatus result;
  {
    const std::scoped_lock lock(mutex_);
    result = statusLocked(spec);
    if (spec.source == ComponentSource::Provisioned || result.state == ComponentState::Installed ||
        result.state == ComponentState::Installing || !input_.fetch)
      return result;
    if (const auto previous = activities_.find(spec.id); previous != activities_.end()) {
      finished = previous->second;
      activities_.erase(previous);
    }
    auto activity = std::make_shared<Activity>();
    activity->running = true;
    activities_[spec.id] = activity;
    activity->worker = std::jthread([this, spec](const std::stop_token& stop) { run(spec, stop); });
    result = statusLocked(spec);
  }
  if (finished && finished->worker.joinable())
    finished->worker.join();
  return result;
}

void DiskComponentHost::run(const ComponentSpec& spec, const std::stop_token& stop)
{
  std::string failure;
  for (const auto& file : spec.files) {
    if (stop.stop_requested())
      break;
    const auto target = input_.modelsDir / file.path;
    if (complete(spec, file, sizeOf(target)))
      continue;
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    std::string reason;
    try {
      reason = input_.fetch({.file = file, .target = target, .stop = stop});
    }
    catch (const std::exception&) {
      reason = kNetwork;
    }
    if (stop.stop_requested())
      break;
    if (!reason.empty() || !complete(spec, file, sizeOf(target))) {
      failure = reason.empty() ? std::string(kNetwork) : reason;
      break;
    }
  }
  const std::scoped_lock lock(mutex_);
  const auto activity = activities_.find(spec.id);
  if (activity == activities_.end())
    return;
  activity->second->running = false;
  activity->second->failure = failure;
}

void DiskComponentHost::stop(const std::string& id)
{
  std::shared_ptr<Activity> activity;
  {
    const std::scoped_lock lock(mutex_);
    const auto found = activities_.find(id);
    if (found == activities_.end())
      return;
    activity = found->second;
    activities_.erase(found);
  }
  activity->worker.request_stop();
  if (activity->worker.joinable())
    activity->worker.join();
}

ComponentStatus DiskComponentHost::cancel(const ComponentSpec& spec)
{
  check(spec);
  stop(spec.id);
  return status(spec);
}

ComponentStatus DiskComponentHost::remove(const ComponentSpec& spec)
{
  check(spec);
  stop(spec.id);
  if (spec.source == ComponentSource::Download) {
    for (const auto& file : spec.files) {
      const auto target = input_.modelsDir / file.path;
      std::error_code error;
      std::filesystem::remove(target, error);
      std::filesystem::remove(withSuffix(target, kPartSuffix), error);
      std::filesystem::remove(withSuffix(target, kSidecarSuffix), error);
    }
  }
  return status(spec);
}
