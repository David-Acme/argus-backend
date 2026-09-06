#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <drogon/drogon.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/vision/vision-service.hxx>
#include <shared/services/vision/remote/vlm-remote.hxx>

namespace
{

cv::Mat probeImage()
{
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
  return img;
}

void directCheck(VisionService& vision)
{
  const cv::Mat img = probeImage();
  for (int i = 0; i < 2; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto caption = vision.describeMat(img);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    std::printf("run %d: %.1f ms -> \"%s\"\n", i + 1, ms, caption.c_str());
  }

  const auto t0 = std::chrono::steady_clock::now();
  const auto answer =
      vision.describeMat(img,
                         "Is there a person in this image? Answer yes or no.",
                         12);
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  std::printf("prompt: %.1f ms -> \"%s\"\n", ms, answer.c_str());
}

void httpCheck(const std::string& url)
{
  const cv::Mat img = probeImage();
  std::vector<unsigned char> jpeg;
  cv::imencode(".jpg", img, jpeg, {cv::IMWRITE_JPEG_QUALITY, 90});
  const std::string imageB64 =
      drogon::utils::base64Encode(jpeg.data(), jpeg.size());

  VlmHttpClient client(url, 120000);
  const std::string prompt = "Can you describe this image?";
  for (int i = 0; i < 2; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto caption = client.describe(imageB64, prompt, "argus-vision-check");
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    std::printf("http run %d: %.1f ms -> \"%s\"\n", i + 1, ms,
                caption.c_str());
  }
}

} // namespace

int main(int argc, char** argv)
{
  std::string httpUrl;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--http") == 0 && i + 1 < argc)
      httpUrl = argv[++i];
  }

  if (!httpUrl.empty()) {
    // Wire mode: no local engine, the probe drives the argus-vlm describe
    // wire exactly like the legacy remote adapter does.
    httpCheck(httpUrl);
    return 0;
  }

  ConfigService::load("config.toml");
  VisionService vision;
  const auto t0 = std::chrono::steady_clock::now();
  vision.init();
  const double initMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
  if (!vision.isLoaded()) {
    std::printf("NOT LOADED\n");
    return 1;
  }
  std::printf("init: %.0f ms\n", initMs);

  directCheck(vision);
  vision.shutdown();
  return 0;
}