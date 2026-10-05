#include <feature/operator/privacy-mask.hxx>

#include <camera/zone-type.hxx>

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <vector>

namespace
{
bool isMask(const OperatorZone& zone)
{
  return zone.points.size() >= 3 && zone.kind == ZoneType::Privacy;
}
}

bool privacy_mask::covers(std::span<const OperatorZone> zones)
{
  return std::ranges::any_of(zones, isMask);
}

bool privacy_mask::apply(cv::Mat& rgb, std::span<const OperatorZone> zones)
{
  if (rgb.empty())
    return false;
  std::vector<std::vector<cv::Point>> polygons;
  for (const auto& zone : zones) {
    if (!isMask(zone))
      continue;
    std::vector<cv::Point> polygon;
    polygon.reserve(zone.points.size());
    for (const auto& [x, y] : zone.points)
      polygon.emplace_back(
          static_cast<int>(std::lround(std::clamp(x, 0.0, 1.0) * rgb.cols)),
          static_cast<int>(std::lround(std::clamp(y, 0.0, 1.0) * rgb.rows)));
    polygons.push_back(std::move(polygon));
  }
  if (polygons.empty())
    return false;
  cv::fillPoly(rgb, polygons, cv::Scalar(0, 0, 0), cv::LINE_8);
  return true;
}
