#pragma once

#include <shared/vocabulary/operator-zone.hxx>

#include <cstdint>
#include <opencv2/core.hpp>
#include <optional>
#include <span>
#include <vector>

namespace privacy_mask
{
[[nodiscard]] bool covers(std::span<const OperatorZone> zones);
bool apply(cv::Mat& rgb, std::span<const OperatorZone> zones);
[[nodiscard]] std::optional<std::vector<uint8_t>> maskedJpeg(const std::vector<uint8_t>& jpeg,
                                                             std::span<const OperatorZone> zones);
}
