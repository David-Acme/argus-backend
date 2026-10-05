#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/vlm/services/jpeg-gate.hxx>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <string>
#include <vector>

namespace
{

std::string encoded(const char* extension, const JpegSize& size)
{
  const cv::Mat image(size.height, size.width, CV_8UC3, cv::Scalar(10, 120, 200));
  std::vector<unsigned char> bytes;
  REQUIRE(cv::imencode(extension, image, bytes));
  return {bytes.begin(), bytes.end()};
}

char highByte(unsigned value)
{
  return static_cast<char>((value >> 8U) & 0xFFU);
}

char lowByte(unsigned value)
{
  return static_cast<char>(value & 0xFFU);
}

std::string forgedHeader(const JpegSize& size)
{
  const auto width = static_cast<unsigned>(size.width);
  const auto height = static_cast<unsigned>(size.height);
  std::string header{"\xFF\xD8\xFF\xC0\x00\x11\x08", 7};
  header.push_back(highByte(height));
  header.push_back(lowByte(height));
  header.push_back(highByte(width));
  header.push_back(lowByte(width));
  header.append(std::string(10, '\0'));
  return header;
}

}

TEST_CASE("the gate reads a JPEG's size from its frame header")
{
  const std::string jpeg = encoded(".jpg", {.width = 64, .height = 48});
  const auto size = jpegSize(jpeg);
  REQUIRE(size.has_value());
  const JpegSize read = size.value_or(JpegSize{.width = 0, .height = 0});
  CHECK(read.width == 64);
  CHECK(read.height == 48);
  const DecodedJpeg decoded = decodeCameraJpeg(jpeg);
  CHECK(decoded.refusal == JpegRefusal::None);
  CHECK(decoded.bgr.cols == 64);
  CHECK(decoded.bgr.rows == 48);
}

TEST_CASE("anything but a JPEG is refused before the decoder sees it")
{
  CHECK(decodeCameraJpeg(encoded(".png", {.width = 16, .height = 16})).refusal == JpegRefusal::NotJpeg);
  CHECK(decodeCameraJpeg("not an image").refusal == JpegRefusal::NotJpeg);
  CHECK(decodeCameraJpeg("").refusal == JpegRefusal::NotJpeg);
}

TEST_CASE("a header that declares a huge frame is refused without decoding")
{
  const std::string bomb = forgedHeader({.width = 30000, .height = 30000});
  const auto size = jpegSize(bomb);
  REQUIRE(size.has_value());
  CHECK(size.value_or(JpegSize{.width = 0, .height = 0}).width == 30000);
  CHECK(decodeCameraJpeg(bomb).refusal == JpegRefusal::TooLarge);
  CHECK(decodeCameraJpeg(forgedHeader({.width = 8000, .height = 8000})).refusal == JpegRefusal::TooLarge);
  CHECK(decodeCameraJpeg(forgedHeader({.width = 4097, .height = 64})).refusal == JpegRefusal::TooLarge);
  CHECK(decodeCameraJpeg(forgedHeader({.width = 64, .height = 4097})).refusal == JpegRefusal::TooLarge);
  CHECK(decodeCameraJpeg(forgedHeader({.width = 4096, .height = 4096})).refusal == JpegRefusal::Undecodable);
  CHECK(decodeCameraJpeg(forgedHeader({.width = 3840, .height = 2160})).refusal == JpegRefusal::Undecodable);
  CHECK(decodeCameraJpeg(forgedHeader({.width = 64, .height = 64})).refusal == JpegRefusal::Undecodable);
}
