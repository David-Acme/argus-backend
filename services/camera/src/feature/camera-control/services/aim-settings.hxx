#pragma once

#include <feature/camera-control/services/aim-math.hxx>

#include <json/value.h>
#include <optional>
#include <string>
#include <vector>

struct AimSettings
{
  bool ptz{false};
  std::string homePreset;
  bool activeLook{true};
  bool followIntruders{true};
  std::vector<NoAimSector> noAimSectors;

  [[nodiscard]] bool lookable() const { return ptz && !homePreset.empty() && activeLook; }
};

struct AimSettingsPatch
{
  std::optional<bool> activeLook;
  std::optional<bool> followIntruders;
  std::optional<std::vector<NoAimSector>> noAimSectors;
};

struct AimSource
{
  const std::string& capabilities;
  const std::string& config;
};

struct AimMerge
{
  const std::string& config;
  const AimSettingsPatch& patch;
};

namespace aim_settings
{
[[nodiscard]] AimSettings parse(const AimSource& source);
[[nodiscard]] std::string merged(const AimMerge& input);
[[nodiscard]] Json::Value toJson(const AimSettings& settings);
[[nodiscard]] Json::Value sectorsToJson(const std::vector<NoAimSector>& sectors);
}
