#include "duress-feed.hxx"

#include <sync/resource-change.hxx>
#include <sync/sync-change.hxx>
#include <text/json-util.hxx>

#include <string>

namespace duress_feed
{

bool changesPhrase(std::string_view payload)
{
  const Json::Value event = json_util::fromString(std::string(payload));
  return event.isObject() && event.get(sync_change::kKindField, "").asString() == sync_change::kKindResource &&
         event.get(sync_change::kDomainField, "").asString() == resourceDomainToString(ResourceDomain::Guard) &&
         event.get(sync_change::kScopeField, "").asString() == resourceScopeToString(ResourceScope::Duress);
}

}
