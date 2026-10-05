#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/camera/dtos/probe-camera-dto.hxx>
#include <json/value.h>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/schemas/camera/camera-schema.hxx>

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace camera_probe
{
Json::Value run(const CameraSchema& camera);

struct StoredSecretsUse
{
  const ProbeCameraDto& body;
  const CameraSchema& stored;
};

[[nodiscard]] bool needsStoredSecrets(const StoredSecretsUse& use);
[[nodiscard]] bool storedAddressMatches(const StoredSecretsUse& use);
}

struct CameraProbeRequest
{
  ProbeCameraDto body;
  int64_t userId{0};
};

class ProbeSlots
{
public:
  class Slot
  {
  public:
    Slot(ProbeSlots& owner, int64_t userId) : owner_(&owner), userId_(userId) {}
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    Slot(Slot&& other) noexcept : owner_(other.owner_), userId_(other.userId_)
    {
      other.owner_ = nullptr;
    }
    Slot& operator=(Slot&&) = delete;
    ~Slot();

  private:
    ProbeSlots* owner_;
    int64_t userId_;
  };

  [[nodiscard]] std::optional<Slot> acquire(int64_t userId);

private:
  std::mutex mutex_;
  std::unordered_set<int64_t> busy_;
};

class CameraProbeService
{
public:
  drogon::Task<Json::Value> probe(CameraProbeRequest request) const;

private:
  static ProbeSlots& slots();

  CameraRepository repository_;
};
