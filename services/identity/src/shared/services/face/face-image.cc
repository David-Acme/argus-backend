#include "face-image.hxx"

#include <algorithm>
#include <array>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <stb_image.h>
#pragma GCC diagnostic pop

namespace
{
constexpr std::array<unsigned char, 3> kJpegMagic{0xFF, 0xD8, 0xFF};
constexpr std::array<unsigned char, 8> kPngMagic{0x89, 0x50, 0x4E, 0x47,
                                                 0x0D, 0x0A, 0x1A, 0x0A};

template <std::size_t N>
bool startsWith(std::string_view bytes, const std::array<unsigned char, N>& magic)
{
  if (bytes.size() < N)
    return false;
  return std::ranges::equal(bytes.substr(0, N), magic, [](char byte, unsigned char expected) {
    return static_cast<unsigned char>(byte) == expected;
  });
}
}

FaceImageFormat face_image::sniff(std::string_view bytes)
{
  if (startsWith(bytes, kJpegMagic))
    return FaceImageFormat::Jpeg;
  if (startsWith(bytes, kPngMagic))
    return FaceImageFormat::Png;
  return FaceImageFormat::Unsupported;
}

bool face_image::acceptsDimensions(const FaceImageDimensions& dimensions)
{
  return dimensions.width > 0 && dimensions.height > 0 &&
         dimensions.width <= kMaxSide && dimensions.height <= kMaxSide &&
         static_cast<std::int64_t>(dimensions.width) * dimensions.height <= kMaxPixels;
}

DecodedImage face_image::decodeToRgb(std::string_view bytes)
{
  if (sniff(bytes) == FaceImageFormat::Unsupported)
    return {};
  int width = 0;
  int height = 0;
  int channels = 0;
  if (stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                            static_cast<int>(bytes.size()), &width, &height,
                            &channels) == 0)
    return {};
  if (!acceptsDimensions({.width = width, .height = height}))
    return {};

  const int flags = std::max(width, height) > kScaledDecodeThreshold
                        ? cv::IMREAD_REDUCED_COLOR_2
                        : cv::IMREAD_COLOR;
  const cv::Mat encoded(1, static_cast<int>(bytes.size()), CV_8UC1,
                        const_cast<char*>(bytes.data()));
  const cv::Mat bgr = cv::imdecode(encoded, flags);
  if (bgr.empty() || bgr.type() != CV_8UC3)
    return {};
  cv::Mat rgb;
  cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
  return {.rgb = std::vector<std::uint8_t>(rgb.data, rgb.data + rgb.total() * 3),
          .width = rgb.cols,
          .height = rgb.rows};
}

std::string face_image::encodeFaceCrop(const FaceCropEncodeInput& input)
{
  if (input.rgbData == nullptr || input.width <= 1 || input.height <= 1 ||
      input.maxSide <= 0)
    return {};
  const float faceWidth = input.x2 - input.x1;
  const float faceHeight = input.y2 - input.y1;
  const float margin = input.margin * std::max(faceWidth, faceHeight);
  const int x1 = std::clamp(static_cast<int>(input.x1 - margin), 0, input.width - 1);
  const int y1 = std::clamp(static_cast<int>(input.y1 - margin), 0, input.height - 1);
  const int x2 = std::clamp(static_cast<int>(input.x2 + margin), x1 + 1, input.width);
  const int y2 = std::clamp(static_cast<int>(input.y2 + margin), y1 + 1, input.height);
  const cv::Mat full(input.height, input.width, CV_8UC3,
                     const_cast<std::uint8_t*>(input.rgbData));
  cv::Mat face;
  cv::cvtColor(full(cv::Rect(x1, y1, x2 - x1, y2 - y1)), face, cv::COLOR_RGB2BGR);
  const int side = std::max(face.cols, face.rows);
  if (side > input.maxSide) {
    const double scale = static_cast<double>(input.maxSide) / side;
    cv::resize(face, face, cv::Size(), scale, scale, cv::INTER_AREA);
  }
  std::vector<uchar> buffer;
  if (!cv::imencode(".jpg", face, buffer, {cv::IMWRITE_JPEG_QUALITY, input.quality}))
    return {};
  return {buffer.begin(), buffer.end()};
}
