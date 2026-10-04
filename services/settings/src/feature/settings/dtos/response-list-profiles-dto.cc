#include "response-list-profiles-dto.hxx"

#include <feature/settings/dtos/setting-names.hxx>

#include <optional>
#include <string>

namespace
{
Json::Value optionalText(const std::optional<std::string>& value)
{
  return value ? Json::Value(*value) : Json::Value(Json::nullValue);
}

Json::Value installJson(const ChoiceState& state)
{
  Json::Value install(Json::objectValue);
  install["availability"] = choiceAvailabilityName(state.availability);
  install["sizeMb"] = state.sizeMb;
  install["hostCommand"] = state.hostCommand;
  return install;
}

Json::Value changeJson(const ProfileChange& change)
{
  Json::Value entry(Json::objectValue);
  entry["key"] = change.key;
  entry["from"] = optionalText(change.from);
  entry["to"] = change.to;
  entry["changed"] = change.changed;
  if (change.apply)
    entry["apply"] = settingApplyName(*change.apply);
  if (change.install)
    entry["install"] = installJson(*change.install);
  return entry;
}

Json::Value ownerJson(const OwnerPreview& owner)
{
  Json::Value entry(Json::objectValue);
  entry["service"] = owner.service;
  entry["reachable"] = owner.reachable;
  Json::Value changes(Json::arrayValue);
  for (const auto& change : owner.changes)
    changes.append(changeJson(change));
  entry["changes"] = std::move(changes);
  return entry;
}

Json::Value profileJson(const ProfilePreview& profile)
{
  Json::Value entry(Json::objectValue);
  entry["id"] = profile.id;
  entry["labelKey"] = profile.labelKey;
  entry["current"] = profile.current;
  Json::Value owners(Json::arrayValue);
  for (const auto& owner : profile.owners)
    owners.append(ownerJson(owner));
  entry["owners"] = std::move(owners);
  return entry;
}

Json::Value ruleJson(const std::optional<RecommendationRule>& rule)
{
  if (!rule)
    return {Json::nullValue};
  Json::Value entry(Json::objectValue);
  entry["profile"] = rule->profile;
  entry["minCores"] = rule->minCores;
  entry["minRamGb"] = rule->minRamGb;
  entry["vectorIsa"] = rule->vectorIsa;
  return entry;
}

Json::Value hardwareJson(const HardwareFacts& hardware)
{
  Json::Value entry(Json::objectValue);
  entry["cores"] = hardware.cores;
  entry["threads"] = hardware.threads;
  entry["ramGb"] = hardware.ramGb;
  entry["isa"] = std::string(cpuIsaName(hardware.isa));
  entry["gpu"] = toString(hardware.gpu);
  return entry;
}

Json::Value firstRunJson(const std::optional<FirstRunState>& state)
{
  if (!state)
    return {Json::nullValue};
  Json::Value entry(Json::objectValue);
  entry["profile"] = state->profile;
  entry["state"] = state->origin == ProfileOrigin::Reverted ? "reverted" : "applied";
  entry["appliedAt"] = Json::Int64{state->appliedAt};
  Json::Value owners(Json::arrayValue);
  for (const auto& owner : state->owners) {
    Json::Value item(Json::objectValue);
    item["service"] = owner.service;
    Json::Value keys(Json::arrayValue);
    for (const auto& key : owner.keys)
      keys.append(key);
    item["keys"] = std::move(keys);
    owners.append(std::move(item));
  }
  entry["owners"] = std::move(owners);
  return entry;
}

Json::Value recommendationJson(const Recommendation& recommendation)
{
  Json::Value entry(Json::objectValue);
  entry["profile"] = recommendation.profile;
  entry["reason"] = std::string(recommendationReasonName(recommendation.reason));
  entry["hardware"] = hardwareJson(recommendation.hardware);
  entry["rule"] = ruleJson(recommendation.rule);
  entry["missed"] = ruleJson(recommendation.missed);
  Json::Value rules(Json::arrayValue);
  for (const auto& rule : recommendation.rules)
    rules.append(ruleJson(rule));
  entry["rules"] = std::move(rules);
  entry["fallback"] = recommendation.fallback;
  return entry;
}
}

Json::Value ResponseListProfilesDto::toJson() const
{
  Json::Value profiles(Json::arrayValue);
  for (const auto& profile : overview.profiles)
    profiles.append(profileJson(profile));
  Json::Value info(Json::objectValue);
  info["profiles"] = std::move(profiles);
  info["recommendation"] = recommendationJson(overview.recommendation);
  info["firstRun"] = firstRunJson(overview.firstRun);
  return info;
}
