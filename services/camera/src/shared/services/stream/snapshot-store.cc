#include <shared/services/stream/snapshot-store.hxx>

#include <utility>

namespace
{
constexpr std::size_t kMaxSnapshotBytes = std::size_t{4} * 1024 * 1024;
}

SnapshotStore& SnapshotStore::instance()
{
  static SnapshotStore store;
  return store;
}

void SnapshotStore::putFrame(SnapshotFrameInput input)
{
  if (input.jpeg.empty() || input.jpeg.size() > kMaxSnapshotBytes)
    return;
  std::scoped_lock lock(mutex_);
  frames_[input.cameraId] = {.jpeg = std::move(input.jpeg), .atMs = input.atMs};
}

void SnapshotStore::forget(int64_t cameraId)
{
  std::scoped_lock lock(mutex_);
  frames_.erase(cameraId);
  crops_.erase(cameraId);
}

void SnapshotStore::putPersonCrop(SnapshotCropInput input)
{
  if (input.jpeg.empty() || input.jpeg.size() > kMaxSnapshotBytes)
    return;
  std::scoped_lock lock(mutex_);
  auto& tracks = crops_[input.cameraId];
  tracks[input.trackId] = {.jpeg = std::move(input.jpeg), .atMs = input.atMs};
  while (tracks.size() > kMaxTracksPerCamera) {
    auto oldest = tracks.begin();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
      if (it->second.atMs < oldest->second.atMs)
        oldest = it;
    }
    tracks.erase(oldest);
  }
}

std::optional<CameraSnapshot> SnapshotStore::frame(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = frames_.find(cameraId);
  if (found == frames_.end())
    return std::nullopt;
  return found->second;
}

std::optional<CameraSnapshot> SnapshotStore::personCrop(const PersonCropKey& key) const
{
  std::scoped_lock lock(mutex_);
  const auto camera = crops_.find(key.cameraId);
  if (camera == crops_.end())
    return std::nullopt;
  const auto track = camera->second.find(key.trackId);
  if (track == camera->second.end())
    return std::nullopt;
  return track->second;
}

std::optional<CameraSnapshot> SnapshotStore::latestPersonCrop(
    int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto camera = crops_.find(cameraId);
  if (camera == crops_.end())
    return std::nullopt;
  const CameraSnapshot* latest = nullptr;
  for (const auto& [trackId, snapshot] : camera->second) {
    if (!latest || snapshot.atMs > latest->atMs)
      latest = &snapshot;
  }
  if (!latest)
    return std::nullopt;
  return *latest;
}

std::vector<CameraSnapshot> SnapshotStore::personCropsSince(const PersonCropWindow& window) const
{
  std::vector<CameraSnapshot> out;
  std::scoped_lock lock(mutex_);
  const auto camera = crops_.find(window.cameraId);
  if (camera == crops_.end())
    return out;
  for (const auto& [trackId, snapshot] : camera->second)
    if (snapshot.atMs >= window.sinceMs)
      out.push_back(snapshot);
  return out;
}
