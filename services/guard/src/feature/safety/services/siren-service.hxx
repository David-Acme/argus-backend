#pragma once

#include <camera/camera-action-client.hxx>
#include <config/guard-config.hxx>
#include <feature/guard/infra/resource-change-outbox.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <mutex>
#include <string>

struct AppSirenInput
{
  int64_t cameraId{0};
  bool enabled{false};
  int64_t actorUserId{0};
  std::string actorIp;
};

struct AppSirenResult
{
  int64_t cameraId{0};
  bool enabled{false};
  int64_t until{0};
};

class SirenService
{
public:
  using Clock = std::function<int64_t()>;

  struct Dependencies
  {
    const CameraActionClient* camera{nullptr};
    const ResourceChangeOutbox* changes{nullptr};
    Clock clock;
  };

  SirenService(Dependencies dependencies, GuardSirenConfig config);

  SirenService(const SirenService&) = delete;
  SirenService& operator=(const SirenService&) = delete;

  void refresh(GuardSirenConfig config);

  [[nodiscard]] drogon::Task<AppSirenResult>
  apply(const AppSirenInput& input) const;

private:
  [[nodiscard]] GuardSirenConfig config() const;

  Dependencies dependencies_;
  mutable std::mutex mutex_;
  GuardSirenConfig config_;
};
