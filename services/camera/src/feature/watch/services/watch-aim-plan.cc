#include "watch-aim-plan.hxx"

#include <cmath>
#include <numbers>

std::string_view aimHoldName(AimHold hold)
{
  switch (hold) {
    case AimHold::None:
      return "none";
    case AimHold::Lease:
      return "lease_held";
    case AimHold::Wear:
      return "wear";
    case AimHold::PositionUnknown:
      return "position_unknown";
    case AimHold::NoAimSector:
      return "no_aim_sector";
  }
  return "none";
}

namespace watch_aim_plan
{
AimHold holdFor(const AimGuardInput& input)
{
  if (!input.leaseHeld)
    return AimHold::Lease;
  if (!input.wearAllows)
    return AimHold::Wear;
  if (input.sectors.empty())
    return AimHold::None;
  if (!input.positionKnown)
    return AimHold::PositionUnknown;
  if (aim_math::entersSector({.from = input.position, .delta = input.delta, .sectors = input.sectors}))
    return AimHold::NoAimSector;
  return AimHold::None;
}

std::optional<double> radarBearingDeg(const RadarTarget& target, double maxRangeMm)
{
  if (target.yMm <= 0.0)
    return std::nullopt;
  const double range = std::hypot(target.xMm, target.yMm);
  if (maxRangeMm > 0.0 && range > maxRangeMm)
    return std::nullopt;
  return std::atan2(target.xMm, target.yMm) * 180.0 / std::numbers::pi;
}

RadarAim radarAim(const RadarAimInput& input)
{
  RadarAim best{.placed = false, .move = false, .targetPan = 0.0, .delta = {}, .sensorId = 0, .reason = "no_target"};
  double bestDistance = 0.0;
  double bestRange = 0.0;
  for (const RadarFix& fix : input.fixes) {
    if (input.maxAgeMs > 0 && input.nowMs - fix.atMs > input.maxAgeMs)
      continue;
    for (const RadarTarget& target : fix.targets) {
      const std::optional<double> bearing = radarBearingDeg(target, input.maxRangeMm);
      if (!bearing)
        continue;
      const double targetPan = *bearing + fix.panOffsetDeg;
      const double distance = input.positionKnown ? std::abs(targetPan - input.position.pan) : std::abs(targetPan);
      const double range = std::hypot(target.xMm, target.yMm);
      if (best.placed && (distance > bestDistance || (distance == bestDistance && range >= bestRange)))
        continue;
      best = {.placed = true,
              .move = false,
              .targetPan = targetPan,
              .delta = {},
              .sensorId = fix.sensorId,
              .reason = {}};
      bestDistance = distance;
      bestRange = range;
    }
  }
  if (!best.placed)
    return best;
  if (!input.positionKnown) {
    best.reason = "position_unknown";
    return best;
  }
  const double pan = best.targetPan - input.position.pan;
  if (std::abs(pan) <= input.deadZoneDeg) {
    best.reason = "facing";
    return best;
  }
  best.move = true;
  best.delta = {.pan = pan, .tilt = 0.0};
  return best;
}
}
