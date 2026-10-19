#include "watch-camera-board.hxx"

#include <shared/services/camera-driver/camera-capabilities.hxx>

#include <algorithm>

WatchCameraFlags WatchCameraBoard::flagsOf(const CameraSchema& camera)
{
  const Json::Value capabilities = camera_capabilities::of(camera);
  return {.id = camera.id,
          .name = camera.name,
          .enabled = camera.isEnabled && !camera.deletedAt.has_value(),
          .watchman = camera.watchman,
          .noAudio = camera.noAudio,
          .talk = capabilities.get("talk", false).asBool(),
          .panTilt = capabilities.get("ptz", false).asBool(),
          .tvExclusion = camera.tvExclusion};
}

void WatchCameraBoard::replace(const std::vector<CameraSchema>& cameras)
{
  std::unordered_map<int64_t, Entry> next;
  next.reserve(cameras.size());
  for (const CameraSchema& camera : cameras)
    next.emplace(camera.id, Entry{.flags = flagsOf(camera), .schema = camera});
  std::scoped_lock lock(mutex_);
  cameras_ = std::move(next);
  loaded_ = true;
}

void WatchCameraBoard::put(const WatchCameraFlags& flags)
{
  std::scoped_lock lock(mutex_);
  cameras_[flags.id] = Entry{.flags = flags, .schema = std::nullopt};
  loaded_ = true;
}

std::optional<WatchCameraFlags> WatchCameraBoard::flags(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = cameras_.find(cameraId);
  if (found == cameras_.end())
    return std::nullopt;
  return found->second.flags;
}

std::optional<CameraSchema> WatchCameraBoard::schema(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = cameras_.find(cameraId);
  if (found == cameras_.end())
    return std::nullopt;
  return found->second.schema;
}

std::vector<WatchCameraFlags> WatchCameraBoard::all() const
{
  std::vector<WatchCameraFlags> out;
  {
    std::scoped_lock lock(mutex_);
    out.reserve(cameras_.size());
    for (const auto& [id, entry] : cameras_)
      out.push_back(entry.flags);
  }
  std::ranges::sort(out, {}, &WatchCameraFlags::id);
  return out;
}

bool WatchCameraBoard::loaded() const
{
  std::scoped_lock lock(mutex_);
  return loaded_;
}
