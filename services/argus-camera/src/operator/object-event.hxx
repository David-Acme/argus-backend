#pragma once

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Payload of the argus.camera.v1.object_detected NATS event.
struct DetectedEventObject
{
  std::string name;
  float confidence{0};
  float x{0};
  float y{0};
  float w{0};
  float h{0};
  int64_t personId{0};
  std::string identity;
  std::string identityState;
  int identifyAttempts{0};
  double scoreMedian{0.0};
  int scoreSamples{0};
  int zoneWindows{0};
  int trackWindows{0};
  double areaSpread{1.0};
  std::string signature;
  int64_t trackId{0};
  int64_t firstSeenMs{0};
  int64_t lastSeenMs{0};
  int64_t dwellMs{0};
  float identityConfidence{0};
  std::string zoneKind;
  std::string observationId;
};

struct ObjectDetectedEvent
{
  int schemaVersion{3};
  std::string eventId;
  int64_t cameraId{0};
  std::string cameraName;
  std::string rule;
  std::string severity;
  bool escalated{false};
  std::optional<int64_t> knownPersonId;
  int64_t capturedAtMs{0};
  int64_t detectedAtMs{0};
  int64_t publishedAtMs{0};
  int64_t dwellMs{0};
  int64_t trackId{0};
  int frameWidth{0};
  int frameHeight{0};
  std::vector<DetectedEventObject> objects;
};

namespace object_event
{
Json::Value toJson(const ObjectDetectedEvent& event);
} // namespace object_event
