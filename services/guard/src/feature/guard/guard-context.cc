#include "guard-context.hxx"

GuardArea guard_context::evaluate(const GuardAreaInput& input)
{
  const GuardCameraContext& camera = input.camera;
  const GuardPosture& posture = input.posture;
  if (!camera.configured)
    return {};
  const bool ownHoursNow =
      !camera.activeHours.empty() &&
      guard_schedule::inWindows(guard_schedule::parseWindows(camera.activeHours),
                                input.local);
  const bool ownHoursApply =
      posture.mode == GuardMode::Home ||
      (posture.occupancy == "closed" && posture.mode == GuardMode::Away);
  const bool workingHours = (posture.staffOnly || posture.publicPresent) &&
                            (cameraRoleIsWorkArea(camera.role) ||
                             camera.publicArea);
  return {.inUse = (ownHoursNow && ownHoursApply) || workingHours,
          .passerby = camera.outdoor && camera.publicArea && !input.inAlertZone};
}
