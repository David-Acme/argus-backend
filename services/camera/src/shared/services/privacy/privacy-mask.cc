#include <shared/services/privacy/privacy-mask.hxx>

#include <camera/zone-type.hxx>

#include <algorithm>
#include <cmath>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

namespace
{
constexpr int kMaskedJpegQuality = 90;

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

std::optional<std::vector<uint8_t>> privacy_mask::maskedJpeg(const std::vector<uint8_t>& jpeg,
                                                             std::span<const OperatorZone> zones)
{
  if (jpeg.empty())
    return std::nullopt;
  cv::Mat picture = cv::imdecode(jpeg, cv::IMREAD_COLOR);
  if (!apply(picture, zones))
    return std::nullopt;
  std::vector<uint8_t> encoded;
  if (!cv::imencode(".jpg", picture, encoded, {cv::IMWRITE_JPEG_QUALITY, kMaskedJpegQuality}) ||
      encoded.empty())
    return std::nullopt;
  return encoded;
}
