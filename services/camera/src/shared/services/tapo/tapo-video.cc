#include "tapo-video.hxx"

#include <algorithm>
#include <cstdlib>
#include <text/json-util.hxx>
#include <utility>

namespace
{
constexpr long kFrameRateFlag = 0x10000;
constexpr int kMaxFrameRate = 120;

Json::Value listOf(const Json::Value& node)
{
  if (node.isArray())
    return node;
  if (node.isString()) {
    const Json::Value parsed = json_util::fromString(node.asString());
    if (parsed.isArray())
      return parsed;
  }
  return {Json::arrayValue};
}

std::string resolutionOf(const Json::Value& node)
{
  std::string value = node.isString() ? node.asString() : std::string();
  std::ranges::replace(value, '*', 'x');
  return value;
}

const Json::Value& mainOf(const Json::Value& section)
{
  static const Json::Value empty;
  if (!section.isObject())
    return empty;
  return section["main"].isObject() ? section["main"] : empty;
}
}

namespace tapo_video
{
int frameRateOf(const Json::Value& code)
{
  long value = 0;
  if (code.isIntegral())
    value = code.asInt64();
  else if (code.isString())
    value = std::strtol(code.asString().c_str(), nullptr, 10);
  if (value >= kFrameRateFlag)
    value %= kFrameRateFlag;
  return value > 0 && value <= kMaxFrameRate ? static_cast<int>(value) : 0;
}

std::optional<TapoVideoProfile> profileOf(const TapoVideoAnswers& answers)
{
  const Json::Value& offered = mainOf(answers.capability["video_capability"]);
  const Json::Value& current = mainOf(answers.quality["video"]);
  if (offered.empty() && current.empty())
    return std::nullopt;

  TapoVideoProfile profile;
  profile.resolution = resolutionOf(current["resolution"]);
  profile.frameRate = frameRateOf(current["frame_rate"]);
  profile.encoding = current["encode_type"].isString() ? current["encode_type"].asString() : "";
  for (const auto& code : listOf(offered["frame_rates"])) {
    if (const int rate = frameRateOf(code); rate > 0)
      profile.frameRates.push_back(rate);
  }
  std::ranges::sort(profile.frameRates);
  const auto [first, last] = std::ranges::unique(profile.frameRates);
  profile.frameRates.erase(first, last);
  for (const auto& resolution : listOf(offered["resolutions"])) {
    if (auto value = resolutionOf(resolution); !value.empty())
      profile.resolutions.push_back(std::move(value));
  }
  return profile;
}

std::optional<std::string> frameRateCodeFor(const Json::Value& capability, int frameRate)
{
  for (const auto& code : listOf(mainOf(capability["video_capability"])["frame_rates"])) {
    if (frameRateOf(code) == frameRate)
      return code.isString() ? code.asString() : std::to_string(code.asInt64());
  }
  return std::nullopt;
}
}

Json::Value TapoVideoProfile::toJson() const
{
  Json::Value out(Json::objectValue);
  out["resolution"] = resolution;
  out["frameRate"] = frameRate;
  out["encoding"] = encoding;
  Json::Value rates(Json::arrayValue);
  for (const int rate : frameRates)
    rates.append(rate);
  out["frameRates"] = std::move(rates);
  Json::Value sizes(Json::arrayValue);
  for (const auto& size : resolutions)
    sizes.append(size);
  out["resolutions"] = std::move(sizes);
  return out;
}
