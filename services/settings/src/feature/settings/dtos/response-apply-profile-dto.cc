#include "response-apply-profile-dto.hxx"

#include <feature/settings/dtos/response-owner-catalog-dto.hxx>
#include <feature/settings/dtos/setting-names.hxx>

#include <array>
#include <string>

namespace
{
constexpr std::array kStatuses{ProfileKeyStatus::Applied, ProfileKeyStatus::Unchanged, ProfileKeyStatus::Rejected,
                               ProfileKeyStatus::Unreachable};

Json::Value resultJson(const ProfileKeyResult& result)
{
  Json::Value entry(Json::objectValue);
  entry["key"] = result.key;
  entry["from"] = result.from ? Json::Value(*result.from) : Json::Value(Json::nullValue);
  entry["to"] = result.to;
  entry["status"] = std::string(profileKeyStatusName(result.status));
  if (result.reason)
    entry["reason"] = rejectionReasonName(*result.reason);
  return entry;
}

Json::Value ownerJson(const OwnerApplyResult& owner)
{
  Json::Value entry(Json::objectValue);
  entry["service"] = owner.service;
  entry["reachable"] = owner.reachable;
  Json::Value results(Json::arrayValue);
  for (const auto& result : owner.results)
    results.append(resultJson(result));
  entry["results"] = std::move(results);
  if (owner.catalog)
    entry["catalog"] = ResponseOwnerCatalogDto{.catalog = *owner.catalog}.toJson();
  return entry;
}

Json::Value summaryJson(const std::vector<OwnerApplyResult>& owners)
{
  Json::Value summary(Json::objectValue);
  for (const auto status : kStatuses)
    summary[std::string(profileKeyStatusName(status))] = 0;
  for (const auto& owner : owners)
    for (const auto& result : owner.results) {
      auto& count = summary[std::string(profileKeyStatusName(result.status))];
      count = count.asInt() + 1;
    }
  return summary;
}
}

Json::Value ResponseApplyProfileDto::toJson() const
{
  Json::Value owners(Json::arrayValue);
  for (const auto& owner : outcome.owners)
    owners.append(ownerJson(owner));
  Json::Value info(Json::objectValue);
  info["profile"] = outcome.profile;
  info["summary"] = summaryJson(outcome.owners);
  info["owners"] = std::move(owners);
  return info;
}
