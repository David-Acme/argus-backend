#pragma once

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Payload of the `argus.camera.v1.object_detected` NATS event (see
// argus-contracts/subjects.md for the frozen contract).
struct DetectedEventObject
{
  std::string name;
  float confidence{0};
  float x{0};
  float y{0};
  float w{0};
  float h{0};
};

struct ObjectDetectedEvent
{
  int64_t cameraId{0};
  std::string cameraName;
  std::string rule;
  std::string severity;
  bool escalated{false};
  std::optional<int64_t> knownPersonId;
  int64_t detectedAtMs{0};
  int frameWidth{0};
  int frameHeight{0};
  std::vector<DetectedEventObject> objects;
};

namespace object_event
{
Json::Value toJson(const ObjectDetectedEvent& event);
} // namespace object_event
