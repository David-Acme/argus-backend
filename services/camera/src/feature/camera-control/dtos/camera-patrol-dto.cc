#include "camera-patrol-dto.hxx"

#include <algorithm>

namespace
{
constexpr int kMalformedHour = -1;

std::vector<std::string> stopsOf(const Json::Value& value)
{
  std::vector<std::string> stops;
  if (!value.isArray())
    return stops;
  stops.reserve(value.size());
  for (const Json::Value& stop : value)
    stops.push_back(stop.isString() ? stop.asString() : std::string());
  return stops;
}

int hourOf(const Json::Value& window, const char* key)
{
  const Json::Value& hour = window[key];
  return hour.isInt() ? hour.asInt() : kMalformedHour;
}

bool validHour(int hour)
{
  return hour >= 0 && hour <= patrol_plan::kLastHour;
}

bool neighboursRepeat(const std::vector<std::string>& stops)
{
  const std::size_t count = stops.size();
  if (count < 2)
    return false;
  for (std::size_t index = 0; index < count; ++index)
    if (stops[index] == stops[(index + 1) % count])
      return true;
  return false;
}
}

CameraPatrolDto CameraPatrolDto::fromJson(const Json::Value& json)
{
  CameraPatrolDto dto;
  if (json.isMember("stops"))
    dto.stops = stopsOf(json["stops"]);
  if (json.isMember("dwellSeconds"))
    dto.dwellSeconds = json["dwellSeconds"].isInt64() ? json["dwellSeconds"].asInt64() : 0;
  if (json.isMember("enabled")) {
    if (json["enabled"].isBool())
      dto.enabled = json["enabled"].asBool();
    else
      dto.enabledMalformed = true;
  }
  if (json.isMember("window")) {
    dto.windowGiven = true;
    const Json::Value& window = json["window"];
    if (window.isObject())
      dto.window = PatrolWindow{.startHour = hourOf(window, "startHour"),
                                .endHour = hourOf(window, "endHour")};
    else if (!window.isNull())
      dto.window = PatrolWindow{.startHour = kMalformedHour, .endHour = kMalformedHour};
  }

  START_VALIDATION(CameraPatrolDto, dto)
  CUSTOM_LAMBDA(stops, [](const CameraPatrolDto& d) -> std::optional<std::string> {
    if (!d.stops)
      return std::nullopt;
    const auto& stops = *d.stops;
    if (stops.size() < patrol_plan::kMinStops || stops.size() > patrol_plan::kMaxStops)
      return "must list between 2 and 16 saved presets";
    const bool blank = std::ranges::any_of(stops, [](const std::string& stop) {
      return stop.empty() || stop.size() > patrol_plan::kMaxStopIdLength;
    });
    if (blank)
      return "each stop is a preset id of 1 to 32 characters";
    if (neighboursRepeat(stops))
      return "two neighbouring stops cannot be the same preset";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(dwellSeconds, [](const CameraPatrolDto& d) -> std::optional<std::string> {
    if (!d.dwellSeconds)
      return std::nullopt;
    if (*d.dwellSeconds < patrol_plan::kMinDwellSeconds || *d.dwellSeconds > patrol_plan::kMaxDwellSeconds)
      return "must be between 5 and 600 seconds";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(enabled, [](const CameraPatrolDto& d) -> std::optional<std::string> {
    if (d.enabledMalformed)
      return "must be true or false";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(window, [](const CameraPatrolDto& d) -> std::optional<std::string> {
    if (!d.window)
      return std::nullopt;
    if (!validHour(d.window->startHour) || !validHour(d.window->endHour))
      return "startHour and endHour are whole hours from 0 to 23";
    if (d.window->startHour == d.window->endHour)
      return "startHour and endHour cannot be the same hour; send null to patrol all day";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
