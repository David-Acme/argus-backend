#pragma once

#include <cstdint>
#include <operator/object-event.hxx>

enum class ObjectEventPublishResult : uint8_t
{
  Recorded = 0,
  Duplicate,
  Suppressed,
  Failed,
};

class IObjectEventSink
{
public:
  virtual ~IObjectEventSink() = default;

  virtual ObjectEventPublishResult
  publish(const ObjectDetectedEvent& event) = 0;
};
