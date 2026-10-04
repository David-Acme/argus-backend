#pragma once

#include <json/value.h>
#include <optional>
#include <string>
#include <vector>

struct TapoVideoProfile
{
  std::string resolution;
  int frameRate{0};
  std::string encoding;
  std::vector<int> frameRates;
  std::vector<std::string> resolutions;

  [[nodiscard]] Json::Value toJson() const;
};

struct TapoVideoAnswers
{
  const Json::Value& capability;
  const Json::Value& quality;
};

namespace tapo_video
{
int frameRateOf(const Json::Value& code);
std::optional<TapoVideoProfile> profileOf(const TapoVideoAnswers& answers);
}
