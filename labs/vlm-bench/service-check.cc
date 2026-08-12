#include <chrono>
#include <cstdio>
#include <opencv2/imgproc.hpp>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/vision/vision-service.hxx>

namespace
{
VisionService gVision;
} // namespace

int main()
{
  ConfigService::load("config.toml");
  auto t0 = std::chrono::steady_clock::now();
  gVision.init();
  const double initMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
  if (!gVision.isLoaded()) {
    std::printf("NOT LOADED\n");
    return 1;
  }
  std::printf("init: %.0f ms\n", initMs);

  cv::Mat img(720, 1280, CV_8UC3, cv::Scalar(30, 40, 55));
  for (int y = 0; y < img.rows; ++y) {
    auto* row = img.ptr<unsigned char>(y);
    for (int x = 0; x < img.cols; ++x) {
      row[x * 3 + 0] = static_cast<unsigned char>((x * 255) / img.cols);
      row[x * 3 + 1] = static_cast<unsigned char>((y * 255) / img.rows);
      row[x * 3 + 2] =
          static_cast<unsigned char>(((x + y) * 127) / (img.cols + img.rows));
    }
  }
  cv::rectangle(img, cv::Rect(200, 140, 320, 360), cv::Scalar(240, 240, 240),
                -1);
  cv::circle(img, cv::Point(850, 360), 100, cv::Scalar(60, 90, 220), -1);
  cv::putText(img, "ARGUS", cv::Point(160, 650), cv::FONT_HERSHEY_SIMPLEX, 2.0,
              cv::Scalar(255, 255, 255), 3);

  for (int i = 0; i < 2; ++i) {
    t0 = std::chrono::steady_clock::now();
    const auto caption = gVision.describeMat(img);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    std::printf("run %d: %.1f ms -> \"%s\"\n", i + 1, ms, caption.c_str());
  }

  t0 = std::chrono::steady_clock::now();
  const auto answer =
      gVision.describeMat(img,
                          "Is there a person in this image? Answer yes or no.",
                          12);
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  std::printf("prompt: %.1f ms -> \"%s\"\n", ms, answer.c_str());

  gVision.shutdown();
  return 0;
}
