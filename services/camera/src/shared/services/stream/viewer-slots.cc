#include "viewer-slots.hxx"

namespace viewer_slots
{

Occupancy occupancyOf(const std::vector<Viewer>& viewers, int64_t cameraId)
{
  Occupancy occupancy;
  for (const Viewer& viewer : viewers) {
    if (viewer.recorder)
      continue;
    ++occupancy.total;
    if (viewer.cameraId == cameraId)
      ++occupancy.perCamera;
  }
  return occupancy;
}

Refusal refusalOf(const Limits& limits, const Occupancy& occupancy,
                  const Request& request)
{
  if (request.recorder)
    return Refusal::None;
  const auto ceiling = [&request](int limit) {
    return request.priority || limit <= 1 ? limit : limit - 1;
  };
  if (limits.total > 0 && occupancy.total >= ceiling(limits.total))
    return Refusal::Total;
  if (limits.perCamera > 0 && occupancy.perCamera >= ceiling(limits.perCamera))
    return Refusal::Camera;
  return Refusal::None;
}

const char* refusalMessage(Refusal refusal)
{
  switch (refusal) {
    case Refusal::Total:
      return kTooManyViewers.data();
    case Refusal::Camera:
      return kTooManyViewersForCamera.data();
    case Refusal::None:
      return nullptr;
  }
  return nullptr;
}

}
