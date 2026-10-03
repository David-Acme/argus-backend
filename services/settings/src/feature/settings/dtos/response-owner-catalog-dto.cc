#include "response-owner-catalog-dto.hxx"

#include <string>

namespace
{
std::string typeName(SettingType type)
{
  switch (type) {
  case SettingType::Toggle: return "toggle";
  case SettingType::Integer: return "integer";
  case SettingType::Decimal: return "decimal";
  case SettingType::Choice: return "choice";
  case SettingType::Text: return "text";
  }
  return "text";
}

std::string levelName(SettingLevel level)
{
  return level == SettingLevel::Basic ? "basic" : "advanced";
}

std::string applyName(SettingApply apply)
{
  switch (apply) {
  case SettingApply::Live: return "live";
  case SettingApply::NextSession: return "nextSession";
  case SettingApply::Restart: return "restart";
  }
  return "restart";
}

Json::Value settingJson(const SettingEntry& entry)
{
  Json::Value setting(Json::objectValue);
  setting["key"] = entry.spec.key;
  setting["group"] = entry.spec.group;
  setting["type"] = typeName(entry.spec.type);
  setting["level"] = levelName(entry.spec.level);
  setting["apply"] = applyName(entry.spec.apply);
  setting["min"] = entry.spec.range.min;
  setting["max"] = entry.spec.range.max;
  setting["step"] = entry.spec.range.step;
  Json::Value choices(Json::arrayValue);
  for (const auto& choice : entry.spec.choices)
    choices.append(choice);
  setting["choices"] = std::move(choices);
  setting["value"] = entry.value;
  setting["fallback"] = entry.spec.fallback;
  return setting;
}
}

Json::Value ResponseOwnerCatalogDto::toJson() const
{
  Json::Value owner(Json::objectValue);
  owner["service"] = catalog.service;
  owner["reachable"] = catalog.reachable;
  Json::Value settings(Json::arrayValue);
  for (const auto& entry : catalog.settings)
    settings.append(settingJson(entry));
  owner["settings"] = std::move(settings);
  return owner;
}
