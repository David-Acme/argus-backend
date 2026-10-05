#include "update-environment-dto.hxx"

#include <feature/guard/guard-schedule.hxx>
#include <validation/validation_dsl.hxx>

namespace
{
constexpr int kMaxWindowsLength = 200;
constexpr int kMaxNameLength = 60;

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

std::optional<std::string> hourError(const std::optional<int>& hour)
{
  if (hour && (*hour < 0 || *hour > 23))
    return "must be between 0 and 23";
  return std::nullopt;
}

std::string trimmed(const std::string& text)
{
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return {};
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}
}

UpdateEnvironmentDto UpdateEnvironmentDto::fromJson(const Json::Value& json)
{
  UpdateEnvironmentDto dto;
  const TypedReader read{.json = json, .invalid = dto.invalidTypes};
  dto.name = read.text("name");
  if (dto.name)
    dto.name = trimmed(*dto.name);
  dto.kind = read.text("kind");
  dto.scheduleEnabled = read.flag("scheduleEnabled");
  dto.asleep = read.text("asleep");
  dto.open = read.text("open");
  dto.staffed = read.text("staffed");
  dto.closedMode = read.text("closedMode");
  dto.digestHour = read.integer("digestHour");
  dto.quietPolicy = read.text("quietPolicy");
  dto.quietStartHour = read.integer("quietStartHour");
  dto.quietEndHour = read.integer("quietEndHour");
  dto.lanPresence = read.flag("lanPresence");

  START_VALIDATION(UpdateEnvironmentDto, dto)
  IS_NOT_EMPTY_OPTIONAL(name)
  MAX_LENGTH_OPTIONAL(name, kMaxNameLength)
  IS_IN_OPTIONAL(kind, "home", "office", "commercial", "restaurant",
                 "warehouse", "outdoor")
  IS_IN_OPTIONAL(closedMode, "away", "armed")
  IS_IN_OPTIONAL(quietPolicy, "inherit", "custom", "off")
  CUSTOM_LAMBDA(body,
                [](const UpdateEnvironmentDto& value)
                    -> std::optional<std::string> {
                  if (value.invalidTypes.empty())
                    return std::nullopt;
                  return "wrong type for " + value.invalidTypes;
                })
  CUSTOM_LAMBDA(asleep,
                [](const UpdateEnvironmentDto& value) {
                  return windowsError(value.asleep);
                })
  CUSTOM_LAMBDA(open,
                [](const UpdateEnvironmentDto& value) {
                  return windowsError(value.open);
                })
  CUSTOM_LAMBDA(staffed,
                [](const UpdateEnvironmentDto& value) {
                  return windowsError(value.staffed);
                })
  CUSTOM_LAMBDA(digestHour,
                [](const UpdateEnvironmentDto& value)
                    -> std::optional<std::string> {
                  if (value.digestHour &&
                      (*value.digestHour < -1 || *value.digestHour > 23))
                    return "must be between -1 and 23";
                  return std::nullopt;
                })
  CUSTOM_LAMBDA(quietStartHour,
                [](const UpdateEnvironmentDto& value) {
                  return hourError(value.quietStartHour);
                })
  CUSTOM_LAMBDA(quietEndHour,
                [](const UpdateEnvironmentDto& value) {
                  return hourError(value.quietEndHour);
                })
  END_VALIDATION()
  return dto;
}
