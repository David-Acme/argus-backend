#pragma once

#include <feature/proximity/services/proximity-ports.hxx>

#include <functional>

class CameraWatchClient;
class IdentityClient;

struct FleetProximityWorldInput
{
  const CameraWatchClient* camera{nullptr};
  const IdentityClient* identity{nullptr};
  std::function<drogon::Task<std::vector<ProximityCameraRoom>>()> rooms;
};

class FleetProximityWorld final : public ProximityWorld
{
public:
  explicit FleetProximityWorld(FleetProximityWorldInput input);

  [[nodiscard]] drogon::Task<std::optional<std::vector<ProximitySighting>>> sightings(int64_t sinceMs) const override;

  [[nodiscard]] drogon::Task<std::vector<ProximityCameraRoom>> cameraRooms() const override;

  [[nodiscard]] drogon::Task<std::optional<std::unordered_set<int64_t>>> locatedUsers() const override;

  [[nodiscard]] drogon::Task<std::optional<int64_t>> userOfPerson(int64_t personId) const override;

private:
  FleetProximityWorldInput input_;
};
