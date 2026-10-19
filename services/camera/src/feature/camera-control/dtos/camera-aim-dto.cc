#include "camera-aim-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <vector>

namespace
{
constexpr size_t kMaxSectors = 8;
constexpr size_t kMaxNameLength = 40;
constexpr double kMaxPan = 180.0;
constexpr double kMaxTilt = 90.0;

std::string sectorProblem(const Json::Value& entry)
{
  if (!entry.isObject())
    return "each sector is an object";
  for (const char* key : {"panFrom", "panTo", "tiltFrom", "tiltTo"})
    if (!entry[key].isNumeric())
      return std::string(key) + " must be a number";
  const double panFrom = entry["panFrom"].asDouble();
  const double panTo = entry["panTo"].asDouble();
  const double tiltFrom = entry["tiltFrom"].asDouble();
  const double tiltTo = entry["tiltTo"].asDouble();
  if (panFrom < -kMaxPan || panTo > kMaxPan || tiltFrom < -kMaxTilt || tiltTo > kMaxTilt)
    return "pan is within -180..180 and tilt within -90..90";
  if (panFrom > panTo || tiltFrom > tiltTo)
    return "a sector starts before it ends";
  if (entry.get("name", "").asString().size() > kMaxNameLength)
    return "a sector name has at most 40 characters";
  return {};
}
}

CameraAimDto CameraAimDto::fromJson(const Json::Value& json)
{
  CameraAimDto dto;
  const auto toggle = [&dto, &json](const char* key, std::optional<bool>& target) {
    if (!json.isMember(key))
      return;
    if (!json[key].isBool()) {
      dto.flagsError = std::string(key) + " must be true or false";
      return;
    }
    target = json[key].asBool();
  };
  toggle("activeLook", dto.patch.activeLook);
  toggle("followIntruders", dto.patch.followIntruders);
  toggle("patrol", dto.patch.patrol);
  if (json.isMember("noAimSectors")) {
    const Json::Value& list = json["noAimSectors"];
    if (!list.isArray())
      dto.sectorsError = "noAimSectors must be a list";
    else if (list.size() > kMaxSectors)
      dto.sectorsError = "at most 8 sectors";
    else {
      std::vector<NoAimSector> sectors;
      for (const auto& entry : list) {
        if (const std::string problem = sectorProblem(entry); !problem.empty()) {
          dto.sectorsError = problem;
          break;
        }
        sectors.push_back({.name = entry.get("name", "").asString(),
                           .panFrom = entry["panFrom"].asDouble(),
                           .panTo = entry["panTo"].asDouble(),
                           .tiltFrom = entry["tiltFrom"].asDouble(),
                           .tiltTo = entry["tiltTo"].asDouble()});
      }
      if (dto.sectorsError.empty())
        dto.patch.noAimSectors = std::move(sectors);
    }
  }

  START_VALIDATION(CameraAimDto, dto)
  CUSTOM_LAMBDA(noAimSectors, [](const CameraAimDto& value) -> std::optional<std::string> {
    if (value.sectorsError.empty())
      return std::nullopt;
    return value.sectorsError;
  })
  CUSTOM_LAMBDA(flags, [](const CameraAimDto& value) -> std::optional<std::string> {
    if (value.flagsError.empty())
      return std::nullopt;
    return value.flagsError;
  })
  END_VALIDATION()
  return dto;
}

CameraLeaseDto CameraLeaseDto::fromJson(const Json::Value& json)
{
  CameraLeaseDto dto;
  dto.action = json.get("action", "").asString();

  START_VALIDATION(CameraLeaseDto, dto)
  IS_IN(action, "hold", "release")
  END_VALIDATION()
  return dto;
}
