#include "response-owner-catalog-dto.hxx"

#include <feature/settings/dtos/setting-names.hxx>

#include <string>

namespace
{
Json::Value choiceStatesJson(const std::vector<ChoiceState>& states)
{
  Json::Value list(Json::arrayValue);
  for (const auto& state : states) {
    Json::Value entry(Json::objectValue);
    entry["choice"] = state.choice;
    entry["availability"] = choiceAvailabilityName(state.availability);
    entry["sizeMb"] = state.sizeMb;
    entry["hostCommand"] = state.hostCommand;
    list.append(std::move(entry));
  }
  return list;
}

Json::Value settingJson(const SettingEntry& entry)
{
  Json::Value setting(Json::objectValue);
  setting["key"] = entry.spec.key;
  setting["group"] = entry.spec.group;
  setting["type"] = settingTypeName(entry.spec.type);
  setting["level"] = settingLevelName(entry.spec.level);
  setting["apply"] = settingApplyName(entry.spec.apply);
  setting["min"] = entry.spec.range.min;
  setting["max"] = entry.spec.range.max;
  setting["step"] = entry.spec.range.step;
  Json::Value choices(Json::arrayValue);
  for (const auto& choice : entry.spec.choices)
    choices.append(choice);
  setting["choices"] = std::move(choices);
  setting["value"] = entry.value;
  setting["fallback"] = entry.spec.fallback;
  if (!entry.choiceStates.empty())
    setting["choiceStates"] = choiceStatesJson(entry.choiceStates);
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
