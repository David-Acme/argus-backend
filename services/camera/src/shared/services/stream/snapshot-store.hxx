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

struct SnapshotFrameInput
{
  int64_t cameraId{0};
  std::string jpeg;
  int64_t atMs{0};
};

struct SnapshotCropInput
{
  int64_t cameraId{0};
  int64_t trackId{0};
  std::string jpeg;
  int64_t atMs{0};
};

class SnapshotStore
{
public:
  static SnapshotStore& instance();

  void putFrame(SnapshotFrameInput input);
  void putPersonCrop(SnapshotCropInput input);

  std::optional<CameraSnapshot> frame(int64_t cameraId) const;
  std::optional<CameraSnapshot> personCrop(int64_t cameraId,
                                           int64_t trackId) const;
  std::optional<CameraSnapshot> latestPersonCrop(int64_t cameraId) const;

  void forget(int64_t cameraId);

private:
  SnapshotStore() = default;

  static constexpr std::size_t kMaxTracksPerCamera = 32;

  mutable std::mutex mutex_;
  std::map<int64_t, CameraSnapshot> frames_;
  std::map<int64_t, std::map<int64_t, CameraSnapshot>> crops_;
};
