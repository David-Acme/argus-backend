#pragma once

#include <settings/module-impact-host.hxx>

#include <cstddef>
#include <functional>
#include <string>

struct CameraModuleImpactInput
{
  std::function<std::size_t()> talkSessions;
  std::function<std::size_t()> liveViews;
};

class CameraModuleImpact final : public ModuleImpactHost
{
public:
  explicit CameraModuleImpact(CameraModuleImpactInput input);

  [[nodiscard]] ModuleImpactReport impact(const std::string& moduleId) const override;

private:
  CameraModuleImpactInput input_;
};
