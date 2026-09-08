#pragma once

#include <operator/object-event.hxx>

#include <string>

// The operator's ONLY output channel.
class IObjectEventSink
{
public:
  virtual ~IObjectEventSink() = default;

  virtual bool publish(const ObjectDetectedEvent& event) = 0;
};
