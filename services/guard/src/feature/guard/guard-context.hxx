#pragma once

#include "guard-schedule.hxx"

#include <feature/guard/repositories/camera-context/camera-context-query.hxx>

#include <ctime>

struct GuardAreaInput
{
  const GuardCameraContext& camera;
  const GuardPosture& posture;
  std::tm local{};
  bool inAlertZone{false};
};

struct GuardArea
{
  bool inUse{false};
  bool passerby{false};
};

namespace guard_context
{

GuardArea evaluate(const GuardAreaInput& input);

}
