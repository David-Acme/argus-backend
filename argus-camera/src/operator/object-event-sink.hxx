#pragma once

#include <operator/object-event.hxx>

#include <string>

// The operator's ONLY output channel (Ruling AF): evaluate rules and publish
// `argus.camera.v1.object_detected`. No path from here ever reaches a camera
// control or audible action.
class IObjectEventSink
{
public:
  virtual ~IObjectEventSink() = default;

  virtual bool publish(const ObjectDetectedEvent& event) = 0;
};
