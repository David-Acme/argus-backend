#pragma once

#include <opencv2/core/mat.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

struct JpegSize
{
  int width{0};
  int height{0};
};

enum class JpegRefusal : std::uint8_t
{
  None = 0,
  NotJpeg,
  TooLarge,
  Undecodable
};

struct DecodedJpeg
{
  cv::Mat bgr;
  JpegRefusal refusal{JpegRefusal::None};
};

inline constexpr int kMaxJpegSide = 8192;
inline constexpr std::int64_t kMaxJpegPixels = std::int64_t{7680} * 4320;

[[nodiscard]] std::optional<JpegSize> jpegSize(std::string_view bytes);

[[nodiscard]] DecodedJpeg decodeCameraJpeg(std::string_view bytes);

void limitDecoderPixels();
