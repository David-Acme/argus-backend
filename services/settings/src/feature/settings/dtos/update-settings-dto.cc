#include "update-settings-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <algorithm>
#include <optional>
#include <string>

namespace
{
constexpr std::size_t kMaxChanges = 64;
constexpr std::size_t kMaxKeyLength = 128;
constexpr std::size_t kMaxValueLength = 512;

bool stringField(const Json::Value& entry, const char* name)
{
  return entry.isMember(name) && entry[name].isString();
}

std::optional<std::string> changeProblem(const UpdateSettingsDto& dto)
{
  if (!dto.wellFormed)
    return "each change must be an object with a string key and a string value";
  if (std::ranges::any_of(dto.changes, [](const SettingChange& change) { return change.key.empty(); }))
    return "a change key must not be empty";
  if (std::ranges::any_of(dto.changes,
                          [](const SettingChange& change) { return change.key.size() > kMaxKeyLength; }))
    return "a change key must be at most 128 characters";
  if (std::ranges::any_of(dto.changes,
                          [](const SettingChange& change) { return change.value.size() > kMaxValueLength; }))
    return "a change value must be at most 512 characters";
  return std::nullopt;
}
}

UpdateSettingsDto UpdateSettingsDto::fromJson(const Json::Value& json)
{
  UpdateSettingsDto dto;
  const Json::Value& changes = json.isObject() ? json["changes"] : Json::Value::nullSingleton();
  if (changes.isArray()) {
    dto.changes.reserve(changes.size());
    for (const auto& entry : changes) {
      if (!entry.isObject() || !stringField(entry, "key") || !stringField(entry, "value")) {
        dto.wellFormed = false;
        continue;
      }
      dto.changes.push_back({.key = entry["key"].asString(), .value = entry["value"].asString()});
    }
  }
  else if (!changes.isNull()) {
    dto.wellFormed = false;
  }

  START_VALIDATION(UpdateSettingsDto, dto)
  ARRAY_NOT_EMPTY(changes, SettingChange)
  MAX_ELEMENTS(changes, SettingChange, kMaxChanges)
  CUSTOM_LAMBDA(changes, changeProblem)
  END_VALIDATION()
  return dto;
}
