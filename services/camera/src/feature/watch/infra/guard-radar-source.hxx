#pragma once

#include <feature/watch/services/watch-aim-plan.hxx>

#include <guard/guard-presence-client.hxx>

#include <cstdint>
#include <memory>
#include <vector>

[[nodiscard]] std::vector<RadarFix> radarFixesOf(const std::vector<argus::guard::v1::SensorTargets>& sensors);

class GuardRadarSource
{
public:
  explicit GuardRadarSource(std::shared_ptr<const GuardPresenceClient> client);

  [[nodiscard]] std::vector<RadarFix> fixes(int64_t cameraId) const;

private:
  std::shared_ptr<const GuardPresenceClient> client_;
};
