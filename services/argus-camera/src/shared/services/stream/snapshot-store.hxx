#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

struct CameraSnapshot
{
  std::string jpeg;
  int64_t atMs{0};
};

// Latest full frame and per-track person crops per camera, for guard assessment.
class SnapshotStore
{
public:
  static SnapshotStore& instance();

  void putFrame(int64_t cameraId, const std::string& jpeg, int64_t atMs);
  void putPersonCrop(int64_t cameraId, int64_t trackId,
                     const std::string& jpeg, int64_t atMs);

  std::optional<CameraSnapshot> frame(int64_t cameraId) const;
  std::optional<CameraSnapshot> personCrop(int64_t cameraId,
                                           int64_t trackId) const;
  std::optional<CameraSnapshot> latestPersonCrop(int64_t cameraId) const;

private:
  SnapshotStore() = default;

  static constexpr std::size_t kMaxTracksPerCamera = 32;

  mutable std::mutex mutex_;
  std::map<int64_t, CameraSnapshot> frames_;
  std::map<int64_t, std::map<int64_t, CameraSnapshot>> crops_;
};
