#pragma once

#include <feature/camera-control/services/aim-math.hxx>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class AimHold : uint8_t
{
  None = 0,
  Lease,
  Wear,
  PositionUnknown,
  NoAimSector
};

[[nodiscard]] std::string_view aimHoldName(AimHold hold);

struct AimGuardInput
{
  AimAngles position;
  bool positionKnown{true};
  AimAngles delta;
  std::span<const NoAimSector> sectors;
  bool wearAllows{true};
  bool leaseHeld{true};
};

struct RadarTarget
{
  double xMm{0.0};
  double yMm{0.0};
};

struct RadarFix
{
  int64_t sensorId{0};
  double panOffsetDeg{0.0};
  std::vector<RadarTarget> targets;
  int64_t atMs{0};
};

struct RadarAimInput
{
  std::span<const RadarFix> fixes;
  AimAngles position;
  bool positionKnown{true};
  int64_t nowMs{0};
  int64_t maxAgeMs{3000};
  double maxRangeMm{6000.0};
  double deadZoneDeg{5.0};
};

struct RadarAim
{
  bool placed{false};
  bool move{false};
  double targetPan{0.0};
  AimAngles delta;
  int64_t sensorId{0};
  std::string reason;
};

namespace watch_aim_plan
{
[[nodiscard]] AimHold holdFor(const AimGuardInput& input);
[[nodiscard]] std::optional<double> radarBearingDeg(const RadarTarget& target, double maxRangeMm);
[[nodiscard]] RadarAim radarAim(const RadarAimInput& input);
}
