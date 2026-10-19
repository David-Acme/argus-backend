#pragma once

#include <feature/camera-control/services/patrol-plan.hxx>

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>
#include <vector>

struct CameraPatrolSchema
{
  int64_t cameraId{0};
  std::vector<std::string> stops;
  int64_t dwellSeconds{patrol_plan::kDefaultDwellSeconds};
  bool isEnabled{true};
  std::optional<PatrolWindow> window;
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;

  CameraPatrolSchema() = default;
  explicit CameraPatrolSchema(const drogon::orm::Row& row);
  [[nodiscard]] Json::Value toJson() const;

  [[nodiscard]] static std::string stopsText(const std::vector<std::string>& stops);
};

struct ActiveCameraPatrol
{
  CameraPatrolSchema patrol;
  bool cameraOnline{false};

  ActiveCameraPatrol() = default;
  explicit ActiveCameraPatrol(const drogon::orm::Row& row);
};
