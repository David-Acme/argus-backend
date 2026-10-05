#pragma once

#include <shared/vocabulary/operator-zone.hxx>

#include <opencv2/core.hpp>
#include <span>

namespace privacy_mask
{
[[nodiscard]] bool covers(std::span<const OperatorZone> zones);
bool apply(cv::Mat& rgb, std::span<const OperatorZone> zones);
}
