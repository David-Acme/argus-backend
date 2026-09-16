#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <operator/camera-operator-service.hxx>
#include <operator/frame-source.hxx>
#include <operator/identity-known-person-matcher.hxx>
#include <operator/known-person-matcher.hxx>
#include <operator/object-event-sink.hxx>
#include <operator/object-event.hxx>
#include <operator/operator-config.hxx>
#include <operator/zone-source.hxx>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <identity/identity-client.hxx>
#include <shared/services/stream/snapshot-store.hxx>

void checkGuardKnownEvent(const Json::Value& event);

class MutablePersonMatcher final : public IKnownPersonMatcher
{
public:
  std::optional<PersonMatch> match(const PersonCrop&) const override
  {
    return next;
  }

  PersonMatch next;
};

class RecognizingIdentityClient final : public IdentityClient
{
public:
  RecognizingIdentityClient() : IdentityClient("localhost:1") {}

  std::optional<argus::identity::v1::IdentifyPersonResponse>
  identifyPerson(const std::string& image) const override
  {
    CHECK_FALSE(image.empty());
    ++calls;
    argus::identity::v1::IdentifyPersonResponse response;
    response.set_matched(true);
    response.set_trusted(true);
    response.set_person_id(7);
    response.set_confidence(0.95F);
    return response;
  }

  mutable int calls{0};
};

namespace
{
IdentityConfig matchConfig(int64_t bestShotMs = 10000)
{
  return IdentityConfig{.identify = true,
                        .autoEnroll = false,
                        .captureClearFaces = true,
                        .minFaceBoxPx = 8,
                        .identifyIntervalMs = 2000,
                        .enrollCooldownMs = 600000,
                        .bestShotMs = bestShotMs,
                        .improveMargin = 0.15,
                        .target = {},
                        .rpcSecret = {}};
}

PersonCrop cropOn(const std::vector<uint8_t>& rgb, int64_t trackId,
                  float x = 10.0F, float y = 10.0F, float w = 20.0F,
                  float h = 30.0F)
{
  return {.cameraId = 1,
          .trackId = trackId,
          .firstSeenMs = 0,
          .rgb = const_cast<uint8_t*>(rgb.data()),
          .width = 64,
          .height = 48,
          .x = x,
          .y = y,
          .w = w,
          .h = h};
}
} // namespace

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

TEST_CASE("an unusable later crop keeps the cached known verdict")
{
  static std::vector<uint8_t> rgb(64 * 48 * 3, 100);
  auto client = std::make_unique<RecognizingIdentityClient>();
  const auto* clientPtr = client.get();
  IdentityKnownPersonMatcher matcher(matchConfig(), std::move(client));

  const PersonCrop good{.cameraId = 1,
                        .trackId = 5,
                        .firstSeenMs = 0,
                        .rgb = const_cast<uint8_t*>(rgb.data()),
                        .width = 64,
                        .height = 48,
                        .x = 10.0F,
                        .y = 10.0F,
                        .w = 40.0F,
                        .h = 40.0F};
  const auto known = matcher.match(good);
  REQUIRE(known.has_value());
  CHECK(known->identity == PersonIdentity::Known);
  CHECK(known->personId == 7);
  CHECK(clientPtr->calls == 1);

  const auto knownAgain = matcher.match(good);
  REQUIRE(knownAgain.has_value());
  CHECK(knownAgain->identity == PersonIdentity::Known);
  CHECK(knownAgain->state == IdentityState::Known);
  CHECK(knownAgain->personId == 7);
  CHECK(clientPtr->calls == 1);

  const auto unusable = matcher.match(cropOn(rgb, 5, 10.0F, 10.0F, 6.0F, 6.0F));
  REQUIRE(unusable.has_value());
  CHECK(unusable->identity == PersonIdentity::Known);
  CHECK(unusable->state == IdentityState::Known);
  CHECK(unusable->personId == 7);
  CHECK(unusable->identifyAttempts >= 1);
  CHECK(clientPtr->calls == 1);
}

TEST_CASE("an unknown verdict can upgrade to known on a better crop")
{
  static std::vector<uint8_t> rgb(64 * 48 * 3, 100);
  class LateRecognizer final : public IdentityClient
  {
  public:
    LateRecognizer() : IdentityClient("localhost:1") {}

    std::optional<argus::identity::v1::IdentifyPersonResponse>
    identifyPerson(const std::string&) const override
    {
      ++calls;
      argus::identity::v1::IdentifyPersonResponse response;
      if (calls >= 2) {
        response.set_matched(true);
        response.set_trusted(true);
        response.set_person_id(9);
        response.set_confidence(0.91F);
      }
      return response;
    }

    mutable int calls{0};
  };
  auto client = std::make_unique<LateRecognizer>();
  IdentityKnownPersonMatcher matcher(matchConfig(60000), std::move(client));

  const PersonCrop blurry{.cameraId = 1,
                          .trackId = 6,
                          .firstSeenMs = 0,
                          .rgb = const_cast<uint8_t*>(rgb.data()),
                          .width = 64,
                          .height = 48,
                          .x = 8.0F,
                          .y = 8.0F,
                          .w = 20.0F,
                          .h = 24.0F};
  const auto unknown = matcher.match(blurry);
  REQUIRE(unknown.has_value());
  CHECK(unknown->identity == PersonIdentity::Unknown);
  CHECK(unknown->state == IdentityState::Unrecognized);
  CHECK(unknown->personId == 0);

  std::vector<uint8_t> detailed(64 * 48 * 3);
  for (size_t row = 0; row < 48; ++row)
    for (size_t column = 0; column < 64; ++column)
      detailed[(row * 64 + column) * 3] = static_cast<uint8_t>((row * 7 + column * 5) % 256);
  const PersonCrop sharp{.cameraId = 1,
                         .trackId = 6,
                         .firstSeenMs = 0,
                         .rgb = detailed.data(),
                         .width = 64,
                         .height = 48,
                         .x = 8.0F,
                         .y = 8.0F,
                         .w = 30.0F,
                         .h = 36.0F};
  const auto upgraded = matcher.match(sharp);
  REQUIRE(upgraded.has_value());
  CHECK(upgraded->identity == PersonIdentity::Known);
  CHECK(upgraded->state == IdentityState::Known);
  CHECK(upgraded->personId == 9);
  CHECK(upgraded->identifyAttempts == 2);
}

ObjectDetectedEvent recognizedThenUnusableCrop()
{
  StubDetector detector;
  RecordingSink sink;
  static std::vector<uint8_t> rgb(64 * 48 * 3, 100);
  auto client = std::make_unique<RecognizingIdentityClient>();
  IdentityKnownPersonMatcher matcher(matchConfig(), std::move(client));

  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.operator_.aggregationWindowMs = 80;
  inputs.operator_.dwellMonitorMs = 0;
  inputs.operator_.dwellNightMs = 0;
  inputs.operator_.dwellAlertMs = 0;
  inputs.operator_.cooldownMs = 60000;
  inputs.operator_.nightStartHour = 22;
  inputs.operator_.nightEndHour = 22;

  CameraOperatorService service(inputs);
  auto frame = rgbFrame();
  DetectedObject identified = personObject();
  identified.w = 8.0F;
  identified.h = 8.0F;
  detector.next = {identified};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  sleepMs(100);
  DetectedObject small = personObject();
  small.w = 6.0F;
  small.h = 6.0F;
  detector.next = {small};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});

  REQUIRE(sink.events.size() == 1);
  return sink.events.front();
}

TEST_CASE("processFrame recognized then small crop publishes a consistent triple")
{
  const ObjectDetectedEvent event = recognizedThenUnusableCrop();
  CHECK(event.rule == "known_person");
  REQUIRE(event.objects.size() == 1);
  const auto& object = event.objects.front();
  CHECK(object.identity == "known");
  CHECK(object.personId == 7);
  CHECK(object.identityState == "known");
}

TEST_CASE("guard treats the camera's recognised triple as known")
{
  const ObjectDetectedEvent event = recognizedThenUnusableCrop();
  checkGuardKnownEvent(object_event::toJson(event));
}

TEST_CASE("the aggregation keeps identity stable across a match downgrade")
{
  class KnownThenUnobservableMatcher final : public IKnownPersonMatcher
  {
  public:
    std::optional<PersonMatch> match(const PersonCrop&) const override
    {
      ++calls;
      if (calls == 1)
        return PersonMatch{.identity = PersonIdentity::Known,
                           .state = IdentityState::Known,
                           .personId = 3,
                           .confidence = 0.9F,
                           .identifyAttempts = 1};
      return PersonMatch{.identity = PersonIdentity::Unknown,
                         .state = IdentityState::Unobservable,
                         .personId = 0,
                         .confidence = 0.0F,
                         .identifyAttempts = 1};
    }

    mutable int calls{0};
  } matcher;

  StubDetector detector;
  RecordingSink sink;
  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
  inputs.operator_.aggregationWindowMs = 80;
  inputs.operator_.dwellMonitorMs = 0;
  inputs.operator_.dwellNightMs = 0;
  inputs.operator_.dwellAlertMs = 0;
  inputs.operator_.cooldownMs = 60000;
  inputs.operator_.nightStartHour = 22;
  inputs.operator_.nightEndHour = 22;

  CameraOperatorService service(inputs);
  auto frame = rgbFrame();
  DetectedObject identified = personObject();
  identified.w = 20.0F;
  identified.h = 20.0F;
  detector.next = {identified};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});
  sleepMs(100);
  DetectedObject smaller = personObject();
  smaller.w = 14.0F;
  smaller.h = 14.0F;
  detector.next = {smaller};
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});

  REQUIRE(sink.events.size() == 1);
  const auto& event = sink.events.front();
  REQUIRE(event.objects.size() == 1);
  CHECK(event.objects.front().personId == 3);
  CHECK(event.objects.front().identity == "known");
  CHECK(event.objects.front().identityState == "known");
}

TEST_CASE("an unknown entry can upgrade to known inside one window")
{
  class UnknownThenKnownMatcher final : public IKnownPersonMatcher
  {
  public:
    std::optional<PersonMatch> match(const PersonCrop&) const override
    {
      ++calls;
      if (calls == 1)
        return PersonMatch{.identity = PersonIdentity::Unknown,
                           .state = IdentityState::Unrecognized,
                           .personId = 0,
                           .confidence = 0.0F,
                           .identifyAttempts = 1};
      return PersonMatch{.identity = PersonIdentity::Known,
                         .state = IdentityState::Known,
                         .personId = 4,
                         .confidence = 0.9F,
                         .identifyAttempts = 2};
    }

    mutable int calls{0};
  } matcher;

  StubDetector detector;
  RecordingSink sink;
  CameraOperatorService::Inputs inputs;
  inputs.dependencies = {&detector, nullptr, &sink, &matcher};
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
  sleepMs(100);
  service.processFrame({.cameraId = 1, .cameraName = "Front", .frame = frame});

  REQUIRE(sink.events.size() == 1);
  const auto& event = sink.events.front();
  REQUIRE(event.objects.size() == 1);
  CHECK(event.objects.front().personId == 4);
  CHECK(event.objects.front().identity == "known");
  CHECK(event.objects.front().identityState == "known");
}

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

class UnrecognizedStubMatcher final : public IKnownPersonMatcher
{
public:
  std::optional<PersonMatch> match(const PersonCrop&) const override
  {
    return PersonMatch{.identity = PersonIdentity::Unknown,
                       .state = IdentityState::Unrecognized,
                       .personId = 0,
                       .confidence = 0.0F,
                       .identifyAttempts = 3};
  }
};

TEST_CASE("identity tri-state and track history flow to the published event")
{
  StubDetector detector;
  RecordingSink sink;
  UnrecognizedStubMatcher matcher;
  FakeZoneSource zones;
  OperatorZone zone;
  zone.cameraId = 1;
  zone.name = "yard";
  zone.kind = "monitor";
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
  const auto& event = sink.events.front();
  CHECK(event.schemaVersion == 3);
  REQUIRE(event.objects.size() == 1);
  const auto& object = event.objects.front();
  CHECK(object.identityState == "unrecognized");
  CHECK(object.identifyAttempts == 3);
  CHECK(object.identity == "unknown");
  CHECK(object.scoreSamples >= 1);
  CHECK(object.trackWindows >= 1);
  CHECK(object.zoneWindows >= 1);
  CHECK(object.scoreMedian > 0.0);
  CHECK(object.areaSpread >= 1.0);
}

TEST_CASE("schemaVersion 3 serializes the observation contract additively")
{
  ObjectDetectedEvent event;
  event.eventId = "1:2:3";
  event.cameraId = 1;
  event.rule = "person_day";
  event.severity = "info";
  DetectedEventObject object;
  object.name = "person";
  object.confidence = 0.9F;
  object.personId = 0;
  object.identityState = "unobservable";
  object.identifyAttempts = 0;
  object.scoreMedian = 0.9;
  object.scoreSamples = 2;
  object.zoneWindows = 1;
  object.trackWindows = 2;
  object.areaSpread = 1.1;
  object.trackId = 4;
  object.firstSeenMs = 100;
  object.lastSeenMs = 200;
  object.dwellMs = 100;
  object.zoneKind = "monitor";
  object.observationId = "1:4:100";
  event.objects.push_back(object);

  const Json::Value json = object_event::toJson(event);
  CHECK(json["schemaVersion"].asInt() == 3);
  const Json::Value& entry = json["objects"][0];
  CHECK(entry["identityState"].asString() == "unobservable");
  CHECK(entry["identifyAttempts"].asInt() == 0);
  CHECK(entry["scoreMedian"].asDouble() == doctest::Approx(0.9));
  CHECK(entry["scoreSamples"].asInt() == 2);
  CHECK(entry["zoneWindows"].asInt() == 1);
  CHECK(entry["trackWindows"].asInt() == 2);
  CHECK(entry["areaSpread"].asDouble() == doctest::Approx(1.1));
  CHECK_FALSE(entry.isMember("identity"));
  CHECK_FALSE(entry.isMember("personId"));
}

TEST_CASE("an encode failure reports unobservable without scanning")
{
  static std::vector<uint8_t> rgb(64 * 48 * 3, 100);
  IdentityKnownPersonMatcher matcher({.identify = true,
                                      .autoEnroll = false,
                                      .captureClearFaces = true,
                                      .minFaceBoxPx = 48,
                                      .identifyIntervalMs = 2000,
                                      .enrollCooldownMs = 600000,
                                      .bestShotMs = 10000,
                                      .improveMargin = 0.15,
                                      .target = {},
                                      .rpcSecret = {}});
  const PersonCrop crop{.cameraId = 1,
                        .trackId = 3,
                        .firstSeenMs = 0,
                        .rgb = rgb.data(),
                        .width = 64,
                        .height = 48,
                        .x = 63.0F,
                        .y = 10.0F,
                        .w = 50.0F,
                        .h = 50.0F};
  const auto match = matcher.match(crop);
  REQUIRE(match.has_value());
  CHECK(match->identity == PersonIdentity::Unknown);
  CHECK(match->state == IdentityState::Unobservable);
  CHECK(match->personId == 0);
  CHECK(match->identifyAttempts == 0);
  const auto again = matcher.match(crop);
  REQUIRE(again.has_value());
  CHECK(again->state == IdentityState::Unobservable);
}

TEST_CASE("disabled identity reports unobservable without scanning")
{
  static std::vector<uint8_t> rgb(64 * 48 * 3, 100);
  IdentityKnownPersonMatcher matcher({.identify = false,
                                      .autoEnroll = false,
                                      .captureClearFaces = true,
                                      .minFaceBoxPx = 48,
                                      .identifyIntervalMs = 2000,
                                      .enrollCooldownMs = 600000,
                                      .bestShotMs = 10000,
                                      .improveMargin = 0.15,
                                      .target = {},
                                      .rpcSecret = {}});
  const PersonCrop crop{.cameraId = 1,
                        .trackId = 2,
                        .firstSeenMs = 0,
                        .rgb = rgb.data(),
                        .width = 64,
                        .height = 48,
                        .x = 10.0F,
                        .y = 10.0F,
                        .w = 20.0F,
                        .h = 30.0F};
  const auto match = matcher.match(crop);
  REQUIRE(match.has_value());
  CHECK(match->identity == PersonIdentity::Unknown);
  CHECK(match->state == IdentityState::Unobservable);
  CHECK(match->personId == 0);
  CHECK(match->identifyAttempts == 0);
  CHECK(identityStateToString(match->state) == "unobservable");
}
