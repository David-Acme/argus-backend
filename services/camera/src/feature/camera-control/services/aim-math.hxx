#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct NormalizedBox
{
  double x{0.0};
  double y{0.0};
  double w{0.0};
  double h{0.0};
};

struct NoAimSector
{
  std::string name;
  double panFrom{0.0};
  double panTo{0.0};
  double tiltFrom{0.0};
  double tiltTo{0.0};
};

struct AimAngles
{
  double pan{0.0};
  double tilt{0.0};
};

struct FieldOfView
{
  double horizontalDeg{77.0};
  double verticalDeg{43.0};
};

struct CenterInput
{
  NormalizedBox box;
  FieldOfView fov;
  double deadZone{0.05};
};

struct TrackSample
{
  double cx{0.0};
  double cy{0.0};
  int64_t atMs{0};
};

struct LeadInput
{
  std::span<const TrackSample> samples;
  int minSamples{3};
  double consistency{0.7};
  int64_t aheadMs{1500};
};

struct SectorInput
{
  AimAngles from;
  AimAngles delta;
  std::span<const NoAimSector> sectors;
};

struct WearInput
{
  std::span<const int64_t> recentMovesMs;
  int64_t nowMs{0};
  int perMinute{6};
  int64_t movesToday{0};
  int64_t dailyCap{400};
};

struct BlackoutColumns
{
  int from{0};
  int to{0};
};

struct BlackoutInput
{
  AimAngles position;
  FieldOfView fov;
  std::span<const NoAimSector> sectors;
  int width{0};
};

namespace aim_math
{
[[nodiscard]] double boxShare(const NormalizedBox& box);
[[nodiscard]] std::optional<AimAngles> centerOffset(const CenterInput& input);
[[nodiscard]] std::optional<NormalizedBox> lead(const LeadInput& input);
[[nodiscard]] bool insideSector(const AimAngles& point, std::span<const NoAimSector> sectors);
[[nodiscard]] bool entersSector(const SectorInput& input);
[[nodiscard]] bool wearAllows(const WearInput& input);
[[nodiscard]] std::vector<BlackoutColumns> blackout(const BlackoutInput& input);
[[nodiscard]] int areaCell(const NormalizedBox& box);
}
