#include "response-owner-catalog-dto.hxx"

#include <feature/settings/dtos/setting-names.hxx>

#include <optional>
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

Json::Value markerJson(const std::optional<ProfileMarker>& marker)
{
  if (!marker || marker->origin == ProfileOrigin::None)
    return {Json::nullValue};
  Json::Value entry(Json::objectValue);
  entry["id"] = marker->id;
  entry["origin"] = std::string(profileOriginToString(marker->origin));
  entry["appliedAt"] = Json::Int64{marker->appliedAt};
  Json::Value keys(Json::arrayValue);
  for (const auto& key : marker->keys)
    keys.append(key);
  entry["keys"] = std::move(keys);
  return entry;
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
  setting["unit"] = entry.spec.unit;
  setting["pendingRestart"] = entry.pendingRestart;
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
  owner["configured"] = catalog.configured;
  owner["configFile"] = catalog.configFile;
  Json::Value capabilities(Json::arrayValue);
  for (const auto& capability : catalog.capabilities)
    capabilities.append(capability);
  owner["capabilities"] = std::move(capabilities);
  owner["profile"] = markerJson(catalog.profile);
  return owner;
}
