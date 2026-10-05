#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class FaceImageFormat : std::uint8_t
{
  Jpeg,
  Png,
  Unsupported
};

struct DecodedImage
{
  std::vector<std::uint8_t> rgb;
  int width{0};
  int height{0};
};

struct FaceImageDimensions
{
  int width{0};
  int height{0};
};

struct FaceCropEncodeInput
{
  const std::uint8_t* rgbData{nullptr};
  int width{0};
  int height{0};
  float x1{0.0F};
  float y1{0.0F};
  float x2{0.0F};
  float y2{0.0F};
  float margin{0.0F};
  int maxSide{0};
  int quality{0};
};

namespace face_image
{
inline constexpr int kMaxSide = 4096;
inline constexpr std::int64_t kMaxPixels = std::int64_t{16} * 1000 * 1000;
inline constexpr int kScaledDecodeThreshold = 2048;
inline constexpr int kPortraitMaxSide = 512;
inline constexpr float kPortraitMargin = 0.6F;
inline constexpr int kPortraitQuality = 90;

[[nodiscard]] FaceImageFormat sniff(std::string_view bytes);
[[nodiscard]] bool acceptsDimensions(const FaceImageDimensions& dimensions);
[[nodiscard]] DecodedImage decodeToRgb(std::string_view bytes);
[[nodiscard]] std::string encodeFaceCrop(const FaceCropEncodeInput& input);
}
