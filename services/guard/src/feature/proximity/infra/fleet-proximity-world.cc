#include "fleet-proximity-world.hxx"

#include <camera/camera-watch-client.hxx>
#include <identity/identity-client.hxx>
#include <runtime/blocking-task.hxx>

#include <utility>

FleetProximityWorld::FleetProximityWorld(FleetProximityWorldInput input) : input_(std::move(input)) {}

drogon::Task<std::optional<std::vector<ProximitySighting>>> FleetProximityWorld::sightings(int64_t sinceMs) const
{
  const CameraWatchClient* camera = input_.camera;
  if (camera == nullptr)
    co_return std::nullopt;
  co_return co_await BlockingTask<std::optional<std::vector<ProximitySighting>>>(
      [camera, sinceMs]() -> std::optional<std::vector<ProximitySighting>> {
        const auto answer = camera->sightings(sinceMs);
        if (!answer)
          return std::nullopt;
        std::vector<ProximitySighting> sightings;
        sightings.reserve(answer->size());
        for (const auto& seen : *answer)
          sightings.push_back({.cameraId = seen.camera_id(),
                               .personId = seen.person_id(),
                               .known = seen.known(),
                               .night = seen.night(),
                               .atMs = seen.at_ms()});
        return sightings;
      });
}

drogon::Task<std::vector<ProximityCameraRoom>> FleetProximityWorld::cameraRooms() const
{
  if (!input_.rooms)
    co_return std::vector<ProximityCameraRoom>{};
  co_return co_await input_.rooms();
}

drogon::Task<std::optional<std::unordered_set<int64_t>>> FleetProximityWorld::locatedUsers() const
{
  const IdentityClient* identity = input_.identity;
  if (identity == nullptr)
    co_return std::nullopt;
  co_return co_await BlockingTask<std::optional<std::unordered_set<int64_t>>>(
      [identity]() -> std::optional<std::unordered_set<int64_t>> {
        const auto answer = identity->listPrivacy();
        if (!answer)
          return std::nullopt;
        std::unordered_set<int64_t> located;
        if (answer->has_household() && !answer->household().location())
          return located;
        for (const auto& entry : answer->users())
          if (!entry.has_choices() || entry.choices().location())
            located.insert(entry.user_id());
        return located;
      });
}

drogon::Task<std::optional<int64_t>> FleetProximityWorld::userOfPerson(int64_t personId) const
{
  const IdentityClient* identity = input_.identity;
  if (identity == nullptr || personId <= 0)
    co_return std::nullopt;
  co_return co_await BlockingTask<std::optional<int64_t>>([identity, personId]() -> std::optional<int64_t> {
    const auto person = identity->getPerson(personId);
    if (!person || !person->userId || *person->userId <= 0)
      return std::nullopt;
    return person->userId;
  });
}
