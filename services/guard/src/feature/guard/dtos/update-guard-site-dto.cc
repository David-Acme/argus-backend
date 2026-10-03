#include "update-guard-site-dto.hxx"

#include <feature/guard/guard-schedule.hxx>
#include <validation/validation_dsl.hxx>

namespace
{
constexpr int kMaxWindowsLength = 200;

struct TypedReader
{
  const Json::Value& json;
  std::string& invalid;

  std::optional<std::string> text(const char* key) const
  {
    if (!json.isMember(key))
      return std::nullopt;
    if (!json[key].isString()) {
      note(key);
      return std::nullopt;
    }
    return json[key].asString();
  }

  std::optional<bool> flag(const char* key) const
  {
    if (!json.isMember(key))
      return std::nullopt;
    if (!json[key].isBool()) {
      note(key);
      return std::nullopt;
    }
    return json[key].asBool();
  }

  std::optional<int> integer(const char* key) const
  {
    if (!json.isMember(key))
      return std::nullopt;
    if (!json[key].isInt()) {
      note(key);
      return std::nullopt;
    }
    return json[key].asInt();
  }

  void note(const char* key) const
  {
    if (!invalid.empty())
      invalid += ", ";
    invalid += key;
  }
};

std::optional<std::string> windowsError(const std::optional<std::string>& spec)
{
  if (!spec)
    return std::nullopt;
  if (static_cast<int>(spec->size()) > kMaxWindowsLength)
    return "is too long";
  if (!guard_schedule::validWindows(*spec))
    return "must look like \"mon-fri 08:00-18:00, sat 10:00-14:00\"";
  return std::nullopt;
}
}

UpdateGuardSiteDto UpdateGuardSiteDto::fromJson(const Json::Value& json)
{
  UpdateGuardSiteDto dto;
  const TypedReader read{.json = json, .invalid = dto.invalidTypes};
  dto.profile = read.text("profile");
  dto.scheduleEnabled = read.flag("scheduleEnabled");
  dto.asleep = read.text("asleep");
  dto.open = read.text("open");
  dto.staffed = read.text("staffed");
  dto.closedMode = read.text("closedMode");
  dto.digestHour = read.integer("digestHour");

  START_VALIDATION(UpdateGuardSiteDto, dto)
  IS_IN_OPTIONAL(profile, "home", "office", "commercial")
  IS_IN_OPTIONAL(closedMode, "away", "armed")
  CUSTOM_LAMBDA(body,
                [](const UpdateGuardSiteDto& value)
                    -> std::optional<std::string> {
                  if (value.invalidTypes.empty())
                    return std::nullopt;
                  return "wrong type for " + value.invalidTypes;
                })
  CUSTOM_LAMBDA(asleep,
                [](const UpdateGuardSiteDto& value) {
                  return windowsError(value.asleep);
                })
  CUSTOM_LAMBDA(open,
                [](const UpdateGuardSiteDto& value) {
                  return windowsError(value.open);
                })
  CUSTOM_LAMBDA(staffed,
                [](const UpdateGuardSiteDto& value) {
                  return windowsError(value.staffed);
                })
  CUSTOM_LAMBDA(digestHour,
                [](const UpdateGuardSiteDto& value)
                    -> std::optional<std::string> {
                  if (value.digestHour &&
                      (*value.digestHour < -1 || *value.digestHour > 23))
                    return "must be between -1 and 23";
                  return std::nullopt;
                })
  END_VALIDATION()
  return dto;
}
