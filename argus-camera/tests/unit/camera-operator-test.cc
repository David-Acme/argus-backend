#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <operator/camera-operator-service.hxx>
#include <operator/frame-source.hxx>
#include <operator/known-person-matcher.hxx>
#include <operator/object-event-sink.hxx>
#include <operator/operator-config.hxx>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace
{
void sleepMs(int64_t ms)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

class StubDetector final : public IObjectDetector
{
public:
  bool isLoaded() const override { return true; }

  std::vector<DetectedObject> detect(const uint8_t*, int, int) override
  {
    return next;
  }

  const std::vector<std::string>& classes() const override { return table; }

  std::vector<DetectedObject> next;
  std::vector<std::string> table{"person", "car"};
};

class RecordingSink final : public IObjectEventSink
{
public:
  bool publish(const ObjectDetectedEvent& event) override
  {
    events.push_back(event);
    return true;
  }

  std::vector<ObjectDetectedEvent> events;
};

CameraFrame rgbFrame(int width = 64, int height = 48)
{
  CameraFrame frame;
  frame.rgb.assign(static_cast<size_t>(width) * height * 3, 100);
  frame.width = width;
  frame.height = height;
  frame.capturedAtMs = 0;
  return frame;
}

DetectedObject personObject()
{
  DetectedObject object;
  object.name = "person";
  object.cls = 0;
  object.confidence = 0.9f;
  object.x = 10;
  object.y = 10;
  object.w = 20;
  object.h = 30;
  return object;
}

DetectedObject carObject()
{
  DetectedObject object;
  object.name = "car";
  object.cls = 2;
  object.confidence = 0.8f;
  object.x = 30;
  object.y = 10;
  object.w = 25;
  object.h = 15;
  return object;
}
} // namespace

TEST_CASE("processFrame aggregates detections over the window")
{
  StubDetector detector;
  RecordingSink sink;
  NoKnownPersonMatcher matcher;

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.objects.maxFpsInference = 2.0;
  inputs.operator_.aggregationWindowMs = 80;
  inputs.operator_.cooldownMs = 60000;
  inputs.operator_.nightStartHour = 22;
  inputs.operator_.nightEndHour = 22;

  CameraOperatorService service(inputs);

  auto frame = rgbFrame();
  detector.next = {personObject()};
  service.processFrame(1, "Front", frame);

  CHECK(sink.events.empty());

  sleepMs(100);
  detector.next = {personObject(), carObject()};
  service.processFrame(1, "Front", frame);

  REQUIRE(sink.events.size() == 1);
  const auto& event = sink.events.front();
  CHECK(event.cameraId == 1);
  CHECK(event.cameraName == "Front");
  CHECK(event.rule == "person_day");
  CHECK(event.severity == "info");
  REQUIRE(event.objects.size() == 2);
  CHECK(event.objects[0].name == "person");
  CHECK(event.objects[1].name == "car");
}

TEST_CASE("the aggregation window keeps the dominant severity")
{
  StubDetector detector;
  RecordingSink sink;
  NoKnownPersonMatcher matcher;

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.operator_.aggregationWindowMs = 80;
  inputs.operator_.cooldownMs = 60000;
  OperatorZone zone;
  zone.cameraId = 1;
  zone.kind = "alert";
  zone.points = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
  inputs.operator_.zones.push_back(zone);

  CameraOperatorService service(inputs);

  auto frame = rgbFrame();
  detector.next = {personObject()};
  service.processFrame(1, "Front", frame);

  CHECK(sink.events.empty());

  sleepMs(100);
  service.processFrame(1, "Front", frame);

  REQUIRE(sink.events.size() == 1);
  CHECK(sink.events.front().rule == "person_in_alert_zone");
  CHECK(sink.events.front().severity == "critical");
}

TEST_CASE("a class in cooldown suppresses the whole next window")
{
  StubDetector detector;
  RecordingSink sink;
  NoKnownPersonMatcher matcher;

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.operator_.aggregationWindowMs = 1;
  inputs.operator_.cooldownMs = 60000;

  CameraOperatorService service(inputs);

  auto frame = rgbFrame();
  detector.next = {personObject()};
  service.processFrame(1, "Front", frame);
  sleepMs(5);
  service.processFrame(1, "Front", frame);

  REQUIRE(sink.events.size() == 1);

  sleepMs(5);
  service.processFrame(1, "Front", frame);
  sleepMs(5);
  service.processFrame(1, "Front", frame);
  CHECK(sink.events.size() == 1);
}

TEST_CASE("no detections and an unloaded detector never publish")
{
  StubDetector detector;
  RecordingSink sink;
  NoKnownPersonMatcher matcher;

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.operator_.aggregationWindowMs = 1;

  CameraOperatorService service(inputs);

  auto frame = rgbFrame();
  detector.next = {};
  service.processFrame(1, "Front", frame);
  sleepMs(5);
  service.processFrame(1, "Front", frame);
  CHECK(sink.events.empty());

  class UnloadedDetector final : public IObjectDetector
  {
  public:
    bool isLoaded() const override { return false; }
    std::vector<DetectedObject> detect(const uint8_t*, int, int) override
    {
      return {};
    }
    const std::vector<std::string>& classes() const override { return table; }
    std::vector<std::string> table{"person"};
  } unloaded;
  inputs.dependencies.detector = &unloaded;
  CameraOperatorService disabled(inputs);
  disabled.processFrame(1, "Front", frame);
  CHECK(sink.events.empty());
}
