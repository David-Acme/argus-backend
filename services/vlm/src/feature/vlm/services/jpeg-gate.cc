#include "jpeg-gate.hxx"

#include <opencv2/imgcodecs.hpp>

#include <cstdlib>
#include <string>
#include <utility>

namespace
{

constexpr unsigned char kMarker = 0xFF;
constexpr unsigned char kStartOfImage = 0xD8;

unsigned byteAt(std::string_view bytes, std::size_t index)
{
  return static_cast<unsigned char>(bytes[index]);
}

bool isStartOfFrame(unsigned marker)
{
  return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

bool standalone(unsigned marker)
{
  return marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7);
}

}

std::optional<JpegSize> jpegSize(std::string_view bytes)
{
  if (bytes.size() < 4 || byteAt(bytes, 0) != kMarker || byteAt(bytes, 1) != kStartOfImage)
    return std::nullopt;
  std::size_t at = 2;
  while (at + 4 <= bytes.size()) {
    if (byteAt(bytes, at) != kMarker)
      return std::nullopt;
    const unsigned marker = byteAt(bytes, at + 1);
    if (marker == kMarker) {
      ++at;
      continue;
    }
    if (standalone(marker)) {
      at += 2;
      continue;
    }
    const std::size_t length = (byteAt(bytes, at + 2) << 8U) | byteAt(bytes, at + 3);
    if (length < 2 || at + 2 + length > bytes.size())
      return std::nullopt;
    if (isStartOfFrame(marker)) {
      if (length < 7)
        return std::nullopt;
      const int height = static_cast<int>((byteAt(bytes, at + 5) << 8U) | byteAt(bytes, at + 6));
      const int width = static_cast<int>((byteAt(bytes, at + 7) << 8U) | byteAt(bytes, at + 8));
      return JpegSize{.width = width, .height = height};
    }
    at += 2 + length;
  }
  return std::nullopt;
}

DecodedJpeg decodeCameraJpeg(std::string_view bytes)
{
  const auto size = jpegSize(bytes);
  if (!size)
    return {.bgr = {}, .refusal = JpegRefusal::NotJpeg};
  if (size->width <= 0 || size->height <= 0 || size->width > kMaxJpegSide ||
      size->height > kMaxJpegSide ||
      std::int64_t{size->width} * size->height > kMaxJpegPixels)
    return {.bgr = {}, .refusal = JpegRefusal::TooLarge};
  const cv::Mat raw(1, static_cast<int>(bytes.size()), CV_8UC1,
                    const_cast<char*>(bytes.data()));
  cv::Mat bgr = cv::imdecode(raw, cv::IMREAD_COLOR);
  if (bgr.empty())
    return {.bgr = {}, .refusal = JpegRefusal::Undecodable};
  return {.bgr = std::move(bgr), .refusal = JpegRefusal::None};
}

void limitDecoderPixels()
{
  const std::string limit = std::to_string(kMaxJpegPixels);
  ::setenv("OPENCV_IO_MAX_IMAGE_PIXELS", limit.c_str(), 0);
}
