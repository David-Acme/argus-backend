#include <shared/services/stream/snapshot-store.hxx>

#include <utility>

namespace
{
constexpr std::size_t kMaxSnapshotBytes = 4 * 1024 * 1024;
} // namespace

SnapshotStore& SnapshotStore::instance()
{
  static SnapshotStore store;
  return store;
}

void SnapshotStore::putFrame(int64_t cameraId, const std::string& jpeg,
                             int64_t atMs)
{
  if (jpeg.empty() || jpeg.size() > kMaxSnapshotBytes)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  frames_[cameraId] = {.jpeg = jpeg, .atMs = atMs};
}

void SnapshotStore::putPersonCrop(int64_t cameraId, int64_t trackId,
                                  const std::string& jpeg, int64_t atMs)
{
  if (jpeg.empty() || jpeg.size() > kMaxSnapshotBytes)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  auto& tracks = crops_[cameraId];
  tracks[trackId] = {.jpeg = jpeg, .atMs = atMs};
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
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = frames_.find(cameraId);
  if (found == frames_.end())
    return std::nullopt;
  return found->second;
}

std::optional<CameraSnapshot> SnapshotStore::personCrop(int64_t cameraId,
                                                        int64_t trackId) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto camera = crops_.find(cameraId);
  if (camera == crops_.end())
    return std::nullopt;
  const auto track = camera->second.find(trackId);
  if (track == camera->second.end())
    return std::nullopt;
  return track->second;
}

std::optional<CameraSnapshot> SnapshotStore::latestPersonCrop(
    int64_t cameraId) const
{
  std::lock_guard<std::mutex> lock(mutex_);
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
