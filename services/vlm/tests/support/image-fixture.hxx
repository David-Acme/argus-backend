#pragma once

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <string>
#include <vector>

namespace
{

std::string jpegFixture(int side = 64)
{
  cv::Mat image(side, side, CV_8UC3, cv::Scalar(40, 90, 200));
  cv::rectangle(image,
                cv::Rect(side / 4, side / 4, side / 2, side / 2),
                cv::Scalar(200, 200, 200), cv::FILLED);
  std::vector<unsigned char> bytes;
  cv::imencode(".jpg", image, bytes);
  return {bytes.begin(), bytes.end()};
}

cv::Mat decodedJpeg(const std::string& jpeg)
{
  const cv::Mat raw(1, static_cast<int>(jpeg.size()), CV_8UC1,
                    const_cast<char*>(jpeg.data()));
  return cv::imdecode(raw, cv::IMREAD_COLOR);
}
}
