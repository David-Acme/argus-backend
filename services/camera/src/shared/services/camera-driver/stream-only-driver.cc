#include "stream-only-driver.hxx"

#include <utility>

StreamOnlyDriver::StreamOnlyDriver(CameraSchema camera) : camera_(std::move(camera)) {}

Json::Value StreamOnlyDriver::capabilities() const
{
  Json::Value out;
  for (const char* control :
       {"ptz", "presets", "talk", "privacy", "led", "dayNight", "motion", "autoTrack", "alarm"})
    out[control] = false;
  out["streamOnly"] = true;
  return out;
}

DriverResult StreamOnlyDriver::status()
{
  Json::Value out;
  out["model"] = camera_.model;
  out["manufacturer"] = camera_.manufacturer;
  out["streamOnly"] = true;
  return {.ok = true, .error = {}, .data = out};
}

DriverResult StreamOnlyDriver::unsupported() const
{
  return DriverResult::failure(cameraDriverToString(camera_.driver) +
                               " cameras stream video only; this control is not available");
}

DriverResult StreamOnlyDriver::presets()
{
  return unsupported();
}

DriverResult StreamOnlyDriver::move(const DriverMoveInput&)
{
  return unsupported();
}

DriverResult StreamOnlyDriver::preset(const DriverPresetInput&)
{
  return unsupported();
}

DriverResult StreamOnlyDriver::settings(const DriverSettingsInput&)
{
  return unsupported();
}

DriverResult StreamOnlyDriver::speak(const DriverSpeakInput&)
{
  return unsupported();
}
