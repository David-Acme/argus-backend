#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/camera/dtos/probe-camera-dto.hxx>
#include <json/value.h>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/schemas/camera/camera-schema.hxx>

namespace camera_probe
{
Json::Value run(const CameraSchema& camera);
}

class CameraProbeService
{
public:
  drogon::Task<Json::Value> probe(const ProbeCameraDto& body) const;

private:
  CameraRepository repository_;
};
