#include <objects/ncnn-object-detector.hxx>
#include <operator/operator-config.hxx>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <shared/wrapper/hardware-profile/hardware-profile.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::vector<uint8_t> loadOrSynthesize(const std::string& imagePath, int width,
                                      int height)
{
  if (!imagePath.empty()) {
    const cv::Mat raw = cv::imread(imagePath, cv::IMREAD_COLOR);
    if (!raw.empty()) {
      cv::Mat rgb;
      cv::cvtColor(raw, rgb, cv::COLOR_BGR2RGB);
      if (rgb.cols != width || rgb.rows != height)
        cv::resize(rgb, rgb, cv::Size(width, height));
      std::vector<uint8_t> data(static_cast<size_t>(width) * height * 3);
      std::memcpy(data.data(), rgb.data, data.size());
      return data;
    }
    std::printf("object-bench: could not read %s; using synthetic frames\n",
                imagePath.c_str());
  }

  std::vector<uint8_t> data(static_cast<size_t>(width) * height * 3);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      auto* px = data.data() + (static_cast<size_t>(y) * width + x) * 3;
      px[0] = static_cast<uint8_t>(x * 255 / width);
      px[1] = static_cast<uint8_t>(y * 255 / height);
      px[2] = static_cast<uint8_t>((x / 32 + y / 32) % 2 ? 200 : 40);
    }
  }
  return data;
}

void bench(const char* tier, ObjectDetectorService& detector,
           const std::vector<uint8_t>& frame, int width, int height,
           int iterations)
{
  if (!detector.isLoaded()) {
    std::printf("object-bench: %-7s detector not loaded (model missing?)\n",
                tier);
    return;
  }

  detector.detect(frame.data(), width, height);

  std::vector<double> samples;
  samples.reserve(iterations);
  size_t lastDetections = 0;
  for (int i = 0; i < iterations; ++i) {
    const auto t0 = Clock::now();
    const auto objects = detector.detect(frame.data(), width, height);
    samples.push_back(msSince(t0));
    lastDetections = objects.size();
  }

  std::sort(samples.begin(), samples.end());
  const double total =
      std::accumulate(samples.begin(), samples.end(), 0.0);
  const double avg = total / samples.size();
  const double p50 = samples[samples.size() * 50 / 100];
  const double p95 = samples[std::min(samples.size() - 1,
                                      samples.size() * 95 / 100)];
  std::printf(
      "object-bench: %-7s backend=%-7s %dx%d %zu iter: avg %.2f ms "
      "(%.1f fps), p50 %.2f ms, p95 %.2f ms, last-frame detections %zu\n",
      tier, detector.backend().c_str(), width, height, samples.size(), avg,
      avg > 0 ? 1000.0 / avg : 0.0, p50, p95, lastDetections);
}
} // namespace

int main(int argc, char** argv)
{
  std::string modelDir = "models/objects";
  std::string imagePath;
  int width = 1280;
  int height = 720;
  int iterations = 100;
  float confidence = 0.45f;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](std::string& out) {
      if (i + 1 < argc)
        out = argv[++i];
    };
    if (arg == "--model")
      next(modelDir);
    else if (arg == "--image")
      next(imagePath);
    else if (arg == "--width")
      width = std::atoi(argv[++i]);
    else if (arg == "--height")
      height = std::atoi(argv[++i]);
    else if (arg == "--iterations")
      iterations = std::atoi(argv[++i]);
    else if (arg == "--conf")
      confidence = std::atof(argv[++i]);
    else if (arg == "--help") {
      std::printf(
          "usage: object-bench [--model DIR] [--image FILE] [--width N] "
          "[--height N] [--iterations N] [--conf F]\n");
      return 0;
    }
  }

  std::printf("object-bench: hardware %s\n",
              HardwareProbe::describe().c_str());

  std::vector<uint8_t> frame =
      loadOrSynthesize(imagePath, width, height);
  if (!imagePath.empty() && frame.empty()) {
    std::fprintf(stderr, "object-bench: no frame to run on\n");
    return 1;
  }

  ObjectDetectorOptions vulkanOptions;
  vulkanOptions.modelDir = modelDir;
  vulkanOptions.classes = operator_config::defaultClasses();
  vulkanOptions.confidence = confidence;
  vulkanOptions.useVulkan = true;
  ObjectDetectorService vulkanDetector(vulkanOptions);
  vulkanDetector.init();
  bench("vulkan", vulkanDetector, frame, width, height, iterations);

  ObjectDetectorOptions cpuOptions = vulkanOptions;
  cpuOptions.useVulkan = false;
  ObjectDetectorService cpuDetector(cpuOptions);
  cpuDetector.init();
  bench("cpu", cpuDetector, frame, width, height, iterations);

  if (!vulkanDetector.isLoaded() && !cpuDetector.isLoaded()) {
    std::printf(
        "object-bench: no model artifacts in %s (run `scripts/setup.sh "
        "camera`); benching nothing\n",
        modelDir.c_str());
    return 1;
  }

  if (!imagePath.empty() && vulkanDetector.isLoaded()) {
    const auto objects = vulkanDetector.detect(frame.data(), width, height);
    std::printf("object-bench: %zu detections on %s:\n", objects.size(),
                imagePath.c_str());
    for (const auto& object : objects)
      std::printf("  %s %.2f (%.0f,%.0f %zux%zu)\n", object.name.c_str(),
                  object.confidence, object.x, object.y,
                  static_cast<size_t>(object.w), static_cast<size_t>(object.h));
  }
  return 0;
}
