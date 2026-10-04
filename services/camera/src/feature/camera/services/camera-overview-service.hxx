#pragma once

#include <auth/user-role.hxx>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <shared/repositories/camera/camera-repository.hxx>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

class CameraOverviewService
{
public:
  static constexpr size_t kRecentEvents = 30;

  drogon::Task<Json::Value> overview(UserRole role) const;

private:
  struct ByteSample
  {
    int64_t bytes{0};
    std::chrono::steady_clock::time_point at;
    int kbps{0};
  };

  drogon::Task<Json::Value> streamStats() const;
  [[nodiscard]] int kbpsOf(const std::string& stream, int64_t bytes) const;

  CameraRepository repository_;
  mutable std::mutex samplesMutex_;
  mutable std::unordered_map<std::string, ByteSample> samples_;
};
