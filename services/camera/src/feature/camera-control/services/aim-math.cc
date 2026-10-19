#include "aim-math.hxx"

#include <algorithm>
#include <cmath>

namespace
{
constexpr int64_t kMinuteMs = 60000;
constexpr int kGridSide = 4;
}

namespace aim_math
{
double boxShare(const NormalizedBox& box)
{
  return std::max(0.0, box.w) * std::max(0.0, box.h);
}

std::optional<AimAngles> centerOffset(const CenterInput& input)
{
  const double dx = input.box.x + input.box.w / 2.0 - 0.5;
  const double dy = input.box.y + input.box.h / 2.0 - 0.5;
  if (std::abs(dx) <= input.deadZone && std::abs(dy) <= input.deadZone)
    return std::nullopt;
  return AimAngles{.pan = std::abs(dx) <= input.deadZone ? 0.0 : dx * input.fov.horizontalDeg,
                   .tilt = std::abs(dy) <= input.deadZone ? 0.0 : -dy * input.fov.verticalDeg};
}

std::optional<NormalizedBox> lead(const LeadInput& input)
{
  const auto count = static_cast<int>(input.samples.size());
  if (count < std::max(2, input.minSamples))
    return std::nullopt;
  std::vector<double> vx;
  std::vector<double> vy;
  for (size_t index = 1; index < input.samples.size(); ++index) {
    const TrackSample& before = input.samples[index - 1];
    const TrackSample& after = input.samples[index];
    const auto elapsed = static_cast<double>(after.atMs - before.atMs);
    if (elapsed <= 0.0)
      return std::nullopt;
    vx.push_back((after.cx - before.cx) / elapsed);
    vy.push_back((after.cy - before.cy) / elapsed);
  }
  const auto agreeing = [&input](const std::vector<double>& velocities) {
    const auto positive = std::ranges::count_if(velocities, [](double v) { return v > 0.0; });
    const auto negative = std::ranges::count_if(velocities, [](double v) { return v < 0.0; });
    const double share = static_cast<double>(std::max(positive, negative)) /
                         static_cast<double>(velocities.size());
    return share >= input.consistency;
  };
  if (!agreeing(vx) && !agreeing(vy))
    return std::nullopt;
  double meanX = 0.0;
  double meanY = 0.0;
  for (size_t index = 0; index < vx.size(); ++index) {
    meanX += vx.at(index);
    meanY += vy.at(index);
  }
  meanX /= static_cast<double>(vx.size());
  meanY /= static_cast<double>(vy.size());
  const TrackSample& last = input.samples.back();
  const auto ahead = static_cast<double>(input.aheadMs);
  return NormalizedBox{.x = std::clamp(last.cx + meanX * ahead, 0.0, 1.0),
                       .y = std::clamp(last.cy + meanY * ahead, 0.0, 1.0),
                       .w = 0.0,
                       .h = 0.0};
}

bool insideSector(const AimAngles& point, std::span<const NoAimSector> sectors)
{
  return std::ranges::any_of(sectors, [&point](const NoAimSector& sector) {
    return point.pan >= sector.panFrom && point.pan <= sector.panTo && point.tilt >= sector.tiltFrom &&
           point.tilt <= sector.tiltTo;
  });
}

bool entersSector(const SectorInput& input)
{
  const AimAngles target{.pan = input.from.pan + input.delta.pan, .tilt = input.from.tilt + input.delta.tilt};
  return insideSector(target, input.sectors);
}

bool wearAllows(const WearInput& input)
{
  if (input.dailyCap > 0 && input.movesToday >= input.dailyCap)
    return false;
  const auto lastMinute = std::ranges::count_if(input.recentMovesMs, [&input](int64_t at) {
    return input.nowMs - at < kMinuteMs;
  });
  return lastMinute < input.perMinute;
}

std::vector<BlackoutColumns> blackout(const BlackoutInput& input)
{
  std::vector<BlackoutColumns> columns;
  if (input.width <= 0 || input.fov.horizontalDeg <= 0.0)
    return columns;
  const double left = input.position.pan - input.fov.horizontalDeg / 2.0;
  const double pxPerDeg = static_cast<double>(input.width) / input.fov.horizontalDeg;
  for (const NoAimSector& sector : input.sectors) {
    const double tiltLow = input.position.tilt - input.fov.verticalDeg / 2.0;
    const double tiltHigh = input.position.tilt + input.fov.verticalDeg / 2.0;
    if (sector.tiltTo < tiltLow || sector.tiltFrom > tiltHigh)
      continue;
    const int from = std::clamp(static_cast<int>(std::floor((sector.panFrom - left) * pxPerDeg)), 0, input.width);
    const int to = std::clamp(static_cast<int>(std::ceil((sector.panTo - left) * pxPerDeg)), 0, input.width);
    if (to > from)
      columns.push_back({.from = from, .to = to});
  }
  return columns;
}

int areaCell(const NormalizedBox& box)
{
  const int column = std::clamp(static_cast<int>((box.x + box.w / 2.0) * kGridSide), 0, kGridSide - 1);
  const int row = std::clamp(static_cast<int>((box.y + box.h / 2.0) * kGridSide), 0, kGridSide - 1);
  return row * kGridSide + column;
}
}
