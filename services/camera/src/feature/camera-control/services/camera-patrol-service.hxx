#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/camera-control/dtos/camera-patrol-dto.hxx>
#include <feature/camera-control/dtos/response-camera-patrol-dto.hxx>
#include <feature/camera-control/repositories/camera-patrol/camera-patrol-repository.hxx>
#include <feature/camera-control/services/camera-control-feature-service.hxx>
#include <json/value.h>
#include <shared/repositories/camera/camera-repository.hxx>
#include <string>
#include <sync/user-action.hxx>
#include <vector>

struct CameraPatrolChangeInput
{
  int64_t cameraId{0};
  CameraPatrolDto body{};
  int64_t actorUserId{0};
  std::string actorIp;
};

struct CameraPatrolRemoveInput
{
  int64_t cameraId{0};
  int64_t actorUserId{0};
  std::string actorIp;
};

class CameraPatrolService
{
public:
  drogon::Task<ResponseCameraPatrolDto> read(int64_t cameraId) const;
  drogon::Task<ResponseCameraPatrolDto> change(const CameraPatrolChangeInput& input) const;
  drogon::Task<ResponseCameraPatrolDto> remove(const CameraPatrolRemoveInput& input) const;

private:
  struct PatrolAudit
  {
    int64_t cameraId{0};
    UserAction action{UserAction::Update};
    Json::Value before;
    Json::Value after;
    int64_t actorUserId{0};
    std::string actorIp;
  };

  drogon::Task<void> requireCamera(int64_t cameraId) const;
  drogon::Task<void> requireSavedStops(int64_t cameraId, const std::vector<std::string>& stops) const;
  static drogon::Task<void> publish(PatrolAudit audit);

  CameraRepository cameraRepository_;
  CameraPatrolRepository patrolRepository_;
  CameraControlFeatureService service_;
};
