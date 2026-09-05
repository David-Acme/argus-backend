// camera-probe: the operator pipeline with NO camera, no NATS, no database.
// Synthetic RGB frames (or a --image file) go through
// CameraOperatorService::processFrame against the real detector when
// models/objects is present, a stub detector otherwise, and the published
// events print as the exact argus.camera.v1.object_detected payloads.
#include <operator/camera-operator-service.hxx>
#include <operator/known-person-matcher.hxx>
#include <operator/object-event.hxx>
#include <operator/object-event-sink.hxx>
#include <operator/operator-config.hxx>
#include <objects/ncnn-object-detector.hxx>
#include <shared/utils/json-util/json-util.hxx>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
class StubDetector final : public IObjectDetector
{
public:
  bool isLoaded() const override { return true; }

  std::vector<DetectedObject> detect(const uint8_t*, int, int) override
  {
    // A synthetic person box center-left and a car center-right.
    DetectedObject person;
    person.name = "person";
    person.cls = 0;
    person.confidence = 0.86f;
    person.x = 100;
    person.y = 80;
    person.w = 120;
    person.h = 320;

    DetectedObject car;
    car.name = "car";
    car.cls = 2;
    car.confidence = 0.61f;
    car.x = 640;
    car.y = 120;
    car.w = 320;
    car.h = 180;
    return {person, car};
  }

  const std::vector<std::string>& classes() const override { return table; }

  std::vector<std::string> table{"person", "bicycle", "car"};
};

class PrintingSink final : public IObjectEventSink
{
public:
  bool publish(const ObjectDetectedEvent& event) override
  {
    std::printf("camera-probe: published %s\n",
                json_util::toString(object_event::toJson(event)).c_str());
    ++published;
    return true;
  }

  int published{0};
};

std::vector<uint8_t> frameData(const std::string& imagePath, int width,
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
    std::printf("camera-probe: could not read %s; using synthetic frames\n",
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
} // namespace

int main(int argc, char** argv)
{
  std::string modelDir = "models/objects";
  std::string imagePath;
  int frames = 4;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--model" && i + 1 < argc)
      modelDir = argv[++i];
    else if (arg == "--image" && i + 1 < argc)
      imagePath = argv[++i];
    else if (arg == "--frames" && i + 1 < argc)
      frames = std::atoi(argv[++i]);
    else if (arg == "--help") {
      std::printf("usage: camera-probe [--model DIR] [--image FILE] "
                  "[--frames N]\n");
      return 0;
    }
  }

  // The real model when installed (scripts/setup.sh camera), the stub
  // otherwise: either way the pipeline runs end to end with no camera.
  ObjectDetectorOptions options;
  options.modelDir = modelDir;
  options.classes = operator_config::defaultClasses();
  ObjectDetectorService realDetector(options);
  realDetector.init();

  StubDetector stubDetector;
  NoKnownPersonMatcher matcher;
  PrintingSink sink;

  CameraOperatorService::Dependencies dependencies;
  dependencies.detector = realDetector.isLoaded()
                              ? static_cast<IObjectDetector*>(&realDetector)
                              : static_cast<IObjectDetector*>(&stubDetector);
  dependencies.sink = &sink;
  dependencies.matcher = &matcher;

  ObjectsConfig objects;
  objects.maxFpsInference = 2.0;
  OperatorConfig operator_;
  operator_.aggregationWindowMs = 0;
  operator_.cooldownMs = 0;

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = dependencies;
  inputs.objects = objects;
  inputs.operator_ = operator_;

  CameraOperatorService service(inputs);

  const int width = 1280;
  const int height = 720;
  const std::vector<uint8_t> frame = frameData(imagePath, width, height);

  std::printf(
      "camera-probe: detector=%s (%s), injecting %d synthetic frames of "
      "%dx%d\n",
      realDetector.isLoaded() ? "yolo26n" : "stub",
      realDetector.isLoaded() ? realDetector.backend().c_str() : "stub",
      frames, width, height);

  for (int i = 0; i < frames; ++i) {
    CameraFrame cameraFrame;
    cameraFrame.rgb = frame;
    cameraFrame.width = width;
    cameraFrame.height = height;
    service.processFrame(1, "Probe camera", cameraFrame);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  std::printf("camera-probe: done, %d event(s) published\n", sink.published);
  return 0;
}
