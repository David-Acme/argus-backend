#include "watch-gate.hxx"

namespace v1 = argus::camera::v1;

v1::WatchRefusal watchGateVerdict(const WatchGateInput& input)
{
  if (!input.known)
    return v1::WATCH_REFUSAL_UNKNOWN_CAMERA;
  if (!input.moduleActive)
    return v1::WATCH_REFUSAL_MODULE_OFF;
  if (!input.enabled)
    return v1::WATCH_REFUSAL_DISABLED;
  if (!input.householdWatchman)
    return v1::WATCH_REFUSAL_WATCHMAN_OFF;
  if (!input.audioAllowed)
    return v1::WATCH_REFUSAL_AUDIO_WITHHELD;
  if (!input.cameraWatchman)
    return v1::WATCH_REFUSAL_CAMERA_OFF;
  if (input.noAudio)
    return v1::WATCH_REFUSAL_NO_AUDIO;
  if (input.masked)
    return v1::WATCH_REFUSAL_MASKED;
  return v1::WATCH_REFUSAL_NONE;
}

std::string_view watchRefusalName(v1::WatchRefusal refusal)
{
  return v1::WatchRefusal_Name(refusal);
}
