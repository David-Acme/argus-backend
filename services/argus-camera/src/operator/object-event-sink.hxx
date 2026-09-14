#pragma once

#include <cstdint>
#include <operator/object-event.hxx>

// Durable enqueue outcome; only Recorded advances camera-side state.
enum class ObjectEventPublishResult : uint8_t
{
  Recorded = 0,
  Duplicate,
  Suppressed,
  Failed,
};

// The operator's ONLY output channel.
class IObjectEventSink
{
public:
  virtual ~IObjectEventSink() = default;

  virtual ObjectEventPublishResult
  publish(const ObjectDetectedEvent& event) = 0;
};
