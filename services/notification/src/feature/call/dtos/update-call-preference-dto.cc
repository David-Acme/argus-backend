#include "update-call-preference-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <algorithm>

namespace
{
constexpr std::size_t kMaxMutedEnvironments = 64;

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

  std::optional<int64_t> wide(const char* key) const
  {
    if (!json.isMember(key))
      return std::nullopt;
    if (!json[key].isInt64()) {
      note(key);
      return std::nullopt;
    }
    return json[key].asInt64();
  }

  std::optional<std::vector<int64_t>> ids(const char* key) const
  {
    if (!json.isMember(key))
      return std::nullopt;
    if (!json[key].isArray()) {
      note(key);
      return std::nullopt;
    }
    std::vector<int64_t> values;
    for (const auto& entry : json[key]) {
      if (!entry.isInt64() || entry.asInt64() <= 0) {
        note(key);
        return std::nullopt;
      }
      if (std::ranges::find(values, entry.asInt64()) == values.end())
        values.push_back(entry.asInt64());
    }
    return values;
  }

  void note(const char* key) const
  {
    if (!invalid.empty())
      invalid += ", ";
    invalid += key;
  }
};

std::optional<std::string> hourError(const std::optional<int>& hour)
{
  if (hour && (*hour < -1 || *hour > 23))
    return "must be between -1 and 23";
  return std::nullopt;
}
}

UpdateCallPreferenceDto UpdateCallPreferenceDto::fromJson(const Json::Value& json)
{
  UpdateCallPreferenceDto dto;
  const TypedReader read{.json = json, .invalid = dto.invalidTypes};
  dto.enabled = read.flag("enabled");
  dto.guardCritical = read.text("guardCritical");
  dto.guardIntruder = read.text("guardIntruder");
  dto.guardEscalation = read.text("guardEscalation");
  dto.guardArrival = read.text("guardArrival");
  dto.agenda = read.text("agenda");
  dto.assistant = read.text("assistant");
  dto.quietStartHour = read.integer("quietStartHour");
  dto.quietEndHour = read.integer("quietEndHour");
  dto.dndUntil = read.wide("dndUntil");
  dto.criticalBypass = read.flag("criticalBypass");
  dto.mutedEnvironmentIds = read.ids("mutedEnvironmentIds");

  START_VALIDATION(UpdateCallPreferenceDto, dto)
  IS_IN_OPTIONAL(guardCritical, "call", "notify", "off")
  IS_IN_OPTIONAL(guardIntruder, "call", "notify", "off")
  IS_IN_OPTIONAL(guardEscalation, "call", "notify", "off")
  IS_IN_OPTIONAL(guardArrival, "call", "notify", "off")
  IS_IN_OPTIONAL(agenda, "call", "notify", "off")
  IS_IN_OPTIONAL(assistant, "call", "notify", "off")
  CUSTOM_LAMBDA(body,
                [](const UpdateCallPreferenceDto& value)
                    -> std::optional<std::string> {
                  if (value.invalidTypes.empty())
                    return std::nullopt;
                  return "wrong type for " + value.invalidTypes;
                })
  CUSTOM_LAMBDA(quietStartHour,
                [](const UpdateCallPreferenceDto& value) {
                  return hourError(value.quietStartHour);
                })
  CUSTOM_LAMBDA(quietEndHour,
                [](const UpdateCallPreferenceDto& value) {
                  return hourError(value.quietEndHour);
                })
  CUSTOM_LAMBDA(dndUntil,
                [](const UpdateCallPreferenceDto& value)
                    -> std::optional<std::string> {
                  if (value.dndUntil && *value.dndUntil < 0)
                    return "must be 0 or an epoch second";
                  return std::nullopt;
                })
  CUSTOM_LAMBDA(mutedEnvironmentIds,
                [](const UpdateCallPreferenceDto& value)
                    -> std::optional<std::string> {
                  if (value.mutedEnvironmentIds &&
                      value.mutedEnvironmentIds->size() > kMaxMutedEnvironments)
                    return "lists at most 64 environments";
                  return std::nullopt;
                })
  END_VALIDATION()
  return dto;
}
