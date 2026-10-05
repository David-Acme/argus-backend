#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/vlm/services/jpeg-gate.hxx>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <string>
#include <vector>

namespace
{

std::string encoded(const char* extension, int width, int height)
{
  const cv::Mat image(height, width, CV_8UC3, cv::Scalar(10, 120, 200));
  std::vector<unsigned char> bytes;
  REQUIRE(cv::imencode(extension, image, bytes));
  return {bytes.begin(), bytes.end()};
}

std::string forgedHeader(int width, int height)
{
  std::string header{"\xFF\xD8\xFF\xC0\x00\x11\x08", 7};
  header.push_back(static_cast<char>((height >> 8) & 0xFF));
  header.push_back(static_cast<char>(height & 0xFF));
  header.push_back(static_cast<char>((width >> 8) & 0xFF));
  header.push_back(static_cast<char>(width & 0xFF));
  header.append(std::string(10, '\0'));
  return header;
}

}

TEST_CASE("the gate reads a JPEG's size from its frame header")
{
  const std::string jpeg = encoded(".jpg", 64, 48);
  const auto size = jpegSize(jpeg);
  REQUIRE(size.has_value());
  CHECK(size->width == 64);
  CHECK(size->height == 48);
  const DecodedJpeg decoded = decodeCameraJpeg(jpeg);
  CHECK(decoded.refusal == JpegRefusal::None);
  CHECK(decoded.bgr.cols == 64);
  CHECK(decoded.bgr.rows == 48);
}

TEST_CASE("anything but a JPEG is refused before the decoder sees it")
{
  CHECK(decodeCameraJpeg(encoded(".png", 16, 16)).refusal == JpegRefusal::NotJpeg);
  CHECK(decodeCameraJpeg("not an image").refusal == JpegRefusal::NotJpeg);
  CHECK(decodeCameraJpeg("").refusal == JpegRefusal::NotJpeg);
}

TEST_CASE("a header that declares a huge frame is refused without decoding")
{
  const std::string bomb = forgedHeader(30000, 30000);
  const auto size = jpegSize(bomb);
  REQUIRE(size.has_value());
  CHECK(size->width == 30000);
  CHECK(decodeCameraJpeg(bomb).refusal == JpegRefusal::TooLarge);
  CHECK(decodeCameraJpeg(forgedHeader(8000, 8000)).refusal == JpegRefusal::TooLarge);
  CHECK(decodeCameraJpeg(forgedHeader(64, 64)).refusal == JpegRefusal::Undecodable);
}
