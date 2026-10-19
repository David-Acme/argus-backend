#pragma once

#include <shared/schemas/camera/camera-schema.hxx>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct WatchCameraFlags
{
  int64_t id{0};
  std::string name;
  bool enabled{false};
  bool watchman{false};
  bool noAudio{false};
  bool talk{false};
  bool panTilt{false};
  bool tvExclusion{false};
};

class WatchCameraBoard
{
public:
  void replace(const std::vector<CameraSchema>& cameras);
  void put(const WatchCameraFlags& flags);

  [[nodiscard]] std::optional<WatchCameraFlags> flags(int64_t cameraId) const;
  [[nodiscard]] std::optional<CameraSchema> schema(int64_t cameraId) const;
  [[nodiscard]] std::vector<WatchCameraFlags> all() const;
  [[nodiscard]] bool loaded() const;

  [[nodiscard]] static WatchCameraFlags flagsOf(const CameraSchema& camera);

private:
  struct Entry
  {
    WatchCameraFlags flags;
    std::optional<CameraSchema> schema;
  };

  mutable std::mutex mutex_;
  std::unordered_map<int64_t, Entry> cameras_;
  bool loaded_{false};
};
