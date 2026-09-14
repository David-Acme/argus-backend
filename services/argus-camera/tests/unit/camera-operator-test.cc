#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <operator/camera-operator-service.hxx>
#include <operator/frame-source.hxx>
#include <operator/known-person-matcher.hxx>
#include <operator/object-event-sink.hxx>
#include <operator/operator-config.hxx>
#include <operator/zone-source.hxx>

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

  std::vector<DetectedObject> detect(const DetectInput&) override
  {
    ++calls;
    return next;
  }

  const std::vector<std::string>& classes() const override { return table; }

  std::vector<DetectedObject> next;
  std::vector<std::string> table{"person", "car"};
  int calls{0};
};

class RecordingSink final : public IObjectEventSink
{
public:
  ObjectEventPublishResult publish(const ObjectDetectedEvent& event) override
  {
    attemptedIds.push_back(event.eventId);
    if (nextResult == ObjectEventPublishResult::Recorded)
      events.push_back(event);
    return nextResult;
  }

  ObjectEventPublishResult nextResult{ObjectEventPublishResult::Recorded};
  std::vector<ObjectDetectedEvent> events;
  std::vector<std::string> attemptedIds;
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
  inputs.operator_.dwellMonitorMs = 0;
  inputs.operator_.dwellNightMs = 0;
  inputs.operator_.dwellAlertMs = 0;
  inputs.operator_.cooldownMs = 60000;
  inputs.operator_.nightStartHour = 22;
  inputs.operator_.nightEndHour = 22;

  CameraOperatorService service(inputs);

  auto frame = rgbFrame();
  detector.next = {personObject()};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});

  CHECK(sink.events.empty());

  sleepMs(100);
  detector.next = {personObject(), carObject()};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});

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

// Recognizes only the left (resident) crop; the intruder stays unknown.
class LeftPersonKnownMatcher final : public IKnownPersonMatcher
{
public:
  std::optional<PersonMatch> match(const PersonCrop& crop) const override
  {
    if (crop.x > 30.0F)
      return std::nullopt;
    return PersonMatch{.identity = PersonIdentity::Known,
                       .personId = 7,
                       .confidence = 0.9F};
  }
};

TEST_CASE("each eligible person gets its own event with its own verdict")
{
  StubDetector detector;
  RecordingSink sink;
  LeftPersonKnownMatcher matcher;

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.operator_.aggregationWindowMs = 80;
  inputs.operator_.dwellMonitorMs = 0;
  inputs.operator_.dwellNightMs = 0;
  inputs.operator_.dwellAlertMs = 0;
  inputs.operator_.cooldownMs = 60000;
  OperatorZone zone;
  zone.cameraId = 1;
  zone.kind = "alert";
  zone.points = {{0.5, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.5, 1.0}};
  inputs.operator_.zones.push_back(zone);

  CameraOperatorService service(inputs);

  auto frame = rgbFrame();
  DetectedObject left = personObject();
  DetectedObject right = personObject();
  right.x = 40;
  detector.next = {left, right};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  sleepMs(100);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});

  REQUIRE(sink.events.size() == 2);
  const ObjectDetectedEvent* resident = nullptr;
  const ObjectDetectedEvent* intruder = nullptr;
  for (const auto& event : sink.events) {
    if (event.rule == "known_person")
      resident = &event;
    else if (event.rule == "person_in_alert_zone")
      intruder = &event;
  }
  REQUIRE(resident != nullptr);
  REQUIRE(intruder != nullptr);
  CHECK(resident->trackId != intruder->trackId);
  CHECK(resident->severity == "info");
  CHECK(intruder->severity == "critical");
  CHECK(intruder->dwellMs > 0);

  const auto primaryOf = [](const ObjectDetectedEvent& event)
      -> const DetectedEventObject* {
    for (const auto& object : event.objects) {
      if (object.name == "person" && object.trackId == event.trackId)
        return &object;
    }
    return nullptr;
  };
  const DetectedEventObject* residentPrimary = primaryOf(*resident);
  const DetectedEventObject* intruderPrimary = primaryOf(*intruder);
  REQUIRE(residentPrimary != nullptr);
  REQUIRE(intruderPrimary != nullptr);
  CHECK(residentPrimary->identity == "known");
  CHECK(residentPrimary->personId == 7);
  CHECK(residentPrimary->zoneKind.empty());
  CHECK(intruderPrimary->identity == "unknown");
  CHECK(intruderPrimary->personId == 0);
  CHECK(intruderPrimary->zoneKind == "alert");
  CHECK(residentPrimary->dwellMs > 0);
  CHECK(intruderPrimary->dwellMs > 0);
}

TEST_CASE("a failed enqueue keeps the pending event and does not advance state")
{
  StubDetector detector;
  RecordingSink sink;
  NoKnownPersonMatcher matcher;

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.operator_.aggregationWindowMs = 50;
  inputs.operator_.dwellMonitorMs = 0;
  inputs.operator_.dwellNightMs = 0;
  inputs.operator_.dwellAlertMs = 0;
  inputs.operator_.cooldownMs = 60000;
  inputs.operator_.nightStartHour = 0;
  inputs.operator_.nightEndHour = 0;

  CameraOperatorService service(inputs);

  auto frame = rgbFrame();
  detector.next = {personObject()};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  sleepMs(60);

  sink.nextResult = ObjectEventPublishResult::Failed;
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  CHECK(sink.events.empty());
  REQUIRE(sink.attemptedIds.size() == 1);

  sink.nextResult = ObjectEventPublishResult::Recorded;
  sleepMs(60);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  REQUIRE(sink.events.size() == 1);
  CHECK(sink.events.front().rule == "person_day");
  // The retry reuses the exact event id and never advances the sequence.
  REQUIRE(sink.attemptedIds.size() == 2);
  CHECK(sink.attemptedIds[1] == sink.attemptedIds[0]);
  CHECK(sink.attemptedIds[1] == sink.events.front().eventId);
}

TEST_CASE("a duplicate outbox result does not advance the cooldown")
{
  StubDetector detector;
  RecordingSink sink;
  NoKnownPersonMatcher matcher;

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.operator_.aggregationWindowMs = 50;
  inputs.operator_.dwellMonitorMs = 0;
  inputs.operator_.dwellNightMs = 0;
  inputs.operator_.dwellAlertMs = 0;
  inputs.operator_.cooldownMs = 60000;
  inputs.operator_.nightStartHour = 0;
  inputs.operator_.nightEndHour = 0;

  CameraOperatorService service(inputs);

  auto frame = rgbFrame();
  detector.next = {personObject()};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  sleepMs(60);

  sink.nextResult = ObjectEventPublishResult::Duplicate;
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  CHECK(sink.events.empty());
  REQUIRE(sink.attemptedIds.size() == 1);

  sink.nextResult = ObjectEventPublishResult::Recorded;
  sleepMs(60);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  sleepMs(60);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  REQUIRE(sink.events.size() == 1);
  CHECK(sink.attemptedIds.size() == 2);
  CHECK(sink.attemptedIds[1] != sink.attemptedIds[0]);
}

TEST_CASE("the aggregation window keeps the dominant severity")
{
  StubDetector detector;
  RecordingSink sink;
  NoKnownPersonMatcher matcher;

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.operator_.aggregationWindowMs = 80;
  inputs.operator_.dwellMonitorMs = 0;
  inputs.operator_.dwellNightMs = 0;
  inputs.operator_.dwellAlertMs = 0;
  inputs.operator_.cooldownMs = 60000;
  OperatorZone zone;
  zone.cameraId = 1;
  zone.kind = "alert";
  zone.points = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
  inputs.operator_.zones.push_back(zone);

  CameraOperatorService service(inputs);

  auto frame = rgbFrame();
  detector.next = {personObject()};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});

  CHECK(sink.events.empty());

  sleepMs(100);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});

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
  inputs.operator_.dwellMonitorMs = 0;
  inputs.operator_.dwellNightMs = 0;
  inputs.operator_.dwellAlertMs = 0;

  CameraOperatorService service(inputs);

  auto frame = rgbFrame();
  detector.next = {personObject()};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  sleepMs(5);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});

  REQUIRE(sink.events.size() == 1);

  sleepMs(5);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  sleepMs(5);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
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
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  sleepMs(5);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  CHECK(sink.events.empty());

  class UnloadedDetector final : public IObjectDetector
  {
  public:
    bool isLoaded() const override { return false; }
    std::vector<DetectedObject> detect(const DetectInput&) override
    {
      return {};
    }
    const std::vector<std::string>& classes() const override { return table; }
    std::vector<std::string> table{"person"};
  } unloaded;
  inputs.dependencies.detector = &unloaded;
  CameraOperatorService disabled(inputs);
  disabled.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  CHECK(sink.events.empty());
}

TEST_CASE("the motion gate skips inference on a static scene")
{
  StubDetector detector;
  RecordingSink sink;
  NoKnownPersonMatcher matcher;

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.operator_.motionGate = true;
  inputs.operator_.motionMinRatio = 0.01;
  inputs.operator_.aggregationWindowMs = 1;
  inputs.operator_.cooldownMs = 60000;
  inputs.operator_.dwellMonitorMs = 0;
  inputs.operator_.dwellNightMs = 0;
  inputs.operator_.dwellAlertMs = 0;

  CameraOperatorService service(inputs);
  auto frame = rgbFrame();
  detector.next = {};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  CHECK(detector.calls == 1);

  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  CHECK(detector.calls == 1);

  auto moved = rgbFrame();
  std::fill(moved.rgb.begin(), moved.rgb.begin() + moved.rgb.size() / 2, 0);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = moved});
  CHECK(detector.calls == 2);
}

TEST_CASE("the static-box filter drops a frozen person")
{
  StubDetector detector;
  RecordingSink sink;
  NoKnownPersonMatcher matcher;

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.operator_.ignoreStaticPersons = true;
  inputs.operator_.staticBoxFrames = 1;
  inputs.operator_.aggregationWindowMs = 1;
  inputs.operator_.cooldownMs = 60000;
  inputs.operator_.dwellMonitorMs = 0;
  inputs.operator_.dwellNightMs = 0;
  inputs.operator_.dwellAlertMs = 0;

  CameraOperatorService service(inputs);
  auto frame = rgbFrame();
  detector.next = {personObject()};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  sleepMs(5);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  CHECK(sink.events.empty());
}

TEST_CASE("parseZonePoints reads the database JSON shape")
{
  const auto points = parseZonePoints(
      R"([{"x":0.1,"y":0.2},{"x":0.9,"y":0.2},{"x":0.9,"y":0.9}])");
  REQUIRE(points.size() == 3);
  CHECK(points[0].first == doctest::Approx(0.1));
  CHECK(points[0].second == doctest::Approx(0.2));

  CHECK(parseZonePoints("not json").empty());
  CHECK(parseZonePoints(R"([{"x":0.5}])").empty());
  CHECK(parseZonePoints("[]").empty());
}

TEST_CASE("StaticZoneSource returns only the camera zones")
{
  OperatorZone first;
  first.cameraId = 1;
  first.name = "door";
  first.kind = "alert";
  first.points = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}};
  OperatorZone second = first;
  second.cameraId = 2;
  StaticZoneSource source({first, second});
  CHECK(source.forCamera(1).size() == 1);
  CHECK(source.forCamera(3).empty());
}

class FakeZoneSource final : public IZoneSource
{
public:
  std::vector<OperatorZone> forCamera(int64_t cameraId) override
  {
    std::vector<OperatorZone> zones;
    for (const auto& zone : all) {
      if (zone.cameraId == cameraId)
        zones.push_back(zone);
    }
    return zones;
  }

  std::vector<OperatorZone> all;
};

TEST_CASE("the operator evaluates zones from its zone source")
{
  StubDetector detector;
  RecordingSink sink;
  NoKnownPersonMatcher matcher;
  FakeZoneSource zones;
  OperatorZone zone;
  zone.cameraId = 1;
  zone.name = "door";
  zone.kind = "alert";
  zone.points = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
  zones.all.push_back(zone);

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher, &zones};
  inputs.operator_.aggregationWindowMs = 1;
  inputs.operator_.cooldownMs = 60000;
  inputs.operator_.dwellMonitorMs = 0;
  inputs.operator_.dwellNightMs = 0;
  inputs.operator_.dwellAlertMs = 0;

  CameraOperatorService service(inputs);
  auto frame = rgbFrame();
  detector.next = {personObject()};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  sleepMs(5);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});

  REQUIRE(sink.events.size() == 1);
  CHECK(sink.events.front().rule == "person_in_alert_zone");
  CHECK(sink.events.front().severity == "critical");
}
