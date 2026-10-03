#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/camera/camera-repository.hxx>

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

struct CameraPresenceSample
{
  int64_t cameraId{0};
  bool reachable{false};
};

class ICameraPresenceSink
{
public:
  virtual ~ICameraPresenceSink() = default;
  virtual drogon::Task<void> record(CameraPresenceSample sample) = 0;
};

class CameraPresenceRecorder final : public ICameraPresenceSink
{
public:
  static constexpr int kMissesBeforeOffline = 2;

  drogon::Task<void> record(CameraPresenceSample sample) override;

private:
  struct Presence
  {
    std::optional<bool> online;
    int misses{0};
  };

  struct PresenceChange
  {
    int64_t cameraId{0};
    bool online{false};
  };

  [[nodiscard]] std::optional<bool> transitionFor(const CameraPresenceSample& sample);
  drogon::Task<bool> persist(PresenceChange change) const;
  void remember(PresenceChange change);

  CameraRepository repository_;
  std::mutex mutex_;
  std::unordered_map<int64_t, Presence> presence_;
};
