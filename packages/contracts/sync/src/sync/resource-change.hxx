#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <drogon/HttpTypes.h>
#include <json/value.h>
#include <optional>
#include <string>
#include <string_view>
#include <sync/sync-change.hxx>
#include <vector>

enum class ResourceDomain : std::uint8_t
{
  Camera = 0,
  Guard,
  Productivity,
  Identity,
  Settings,
  Activity
};

enum class ResourceScope : std::uint8_t
{
  Device = 0,
  Zones,
  Status,
  Overview,
  Environments,
  Incidents,
  Episodes,
  ExpectedVisits,
  Responses,
  SafetyPin,
  Duress,
  Projects,
  Calendar,
  PrivacyHousehold,
  PrivacyChoice,
  Visitors,
  Values,
  Voiceprints,
  Presence,
  Entries
};


struct ResourceRoute
{
  ResourceDomain domain;
  ResourceScope scope;
  std::string_view path;
  drogon::HttpMethod method;
};

inline constexpr std::array<ResourceRoute, 20> kResourceRoutes{{
    {.domain = ResourceDomain::Camera,
     .scope = ResourceScope::Device,
     .path = "/camera",
     .method = drogon::Get},
    {.domain = ResourceDomain::Camera,
     .scope = ResourceScope::Zones,
     .path = "/zone",
     .method = drogon::Get},
    {.domain = ResourceDomain::Camera,
     .scope = ResourceScope::Status,
     .path = "/camera/0/status",
     .method = drogon::Get},
    {.domain = ResourceDomain::Camera,
     .scope = ResourceScope::Overview,
     .path = "/camera/overview",
     .method = drogon::Get},
    {.domain = ResourceDomain::Guard,
     .scope = ResourceScope::Environments,
     .path = "/guard/environments",
     .method = drogon::Get},
    {.domain = ResourceDomain::Guard,
     .scope = ResourceScope::Incidents,
     .path = "/guard/incidents",
     .method = drogon::Get},
    {.domain = ResourceDomain::Guard,
     .scope = ResourceScope::Episodes,
     .path = "/guard/episodes",
     .method = drogon::Get},
    {.domain = ResourceDomain::Guard,
     .scope = ResourceScope::ExpectedVisits,
     .path = "/guard/expected-guests",
     .method = drogon::Get},
    {.domain = ResourceDomain::Guard,
     .scope = ResourceScope::Responses,
     .path = "/guard/environments/0/response",
     .method = drogon::Get},
    {.domain = ResourceDomain::Guard,
     .scope = ResourceScope::SafetyPin,
     .path = "/guard/safety",
     .method = drogon::Get},
    {.domain = ResourceDomain::Guard,
     .scope = ResourceScope::Duress,
     .path = "/guard/safety",
     .method = drogon::Get},
    {.domain = ResourceDomain::Productivity,
     .scope = ResourceScope::Projects,
     .path = "/project",
     .method = drogon::Get},
    {.domain = ResourceDomain::Productivity,
     .scope = ResourceScope::Calendar,
     .path = "/calendar-event",
     .method = drogon::Get},
    {.domain = ResourceDomain::Identity,
     .scope = ResourceScope::PrivacyHousehold,
     .path = "/privacy/users",
     .method = drogon::Get},
    {.domain = ResourceDomain::Identity,
     .scope = ResourceScope::PrivacyChoice,
     .path = "/privacy/me",
     .method = drogon::Get},
    {.domain = ResourceDomain::Identity,
     .scope = ResourceScope::Visitors,
     .path = "/visitor",
     .method = drogon::Get},
    {.domain = ResourceDomain::Settings,
     .scope = ResourceScope::Values,
     .path = "/settings",
     .method = drogon::Get},
    {.domain = ResourceDomain::Identity,
     .scope = ResourceScope::Voiceprints,
     .path = "/voiceprint/users",
     .method = drogon::Get},
    {.domain = ResourceDomain::Guard,
     .scope = ResourceScope::Presence,
     .path = "/guard/presence",
     .method = drogon::Get},
    {.domain = ResourceDomain::Activity,
     .scope = ResourceScope::Entries,
     .path = "/sync/activity",
     .method = drogon::Get},
}};

static_assert(std::ranges::all_of(kResourceRoutes,
                                  [](const ResourceRoute& route) {
                                    return route.method == drogon::Get;
                                  }));

inline std::string resourceDomainToString(ResourceDomain domain)
{
  switch (domain) {
    case ResourceDomain::Camera:
      return "camera";
    case ResourceDomain::Guard:
      return "guard";
    case ResourceDomain::Productivity:
      return "productivity";
    case ResourceDomain::Identity:
      return "identity";
    case ResourceDomain::Settings:
      return "settings";
    case ResourceDomain::Activity:
      return "activity";
  }
  return "camera";
}

inline std::optional<ResourceDomain> resourceDomainFromString(std::string_view name)
{
  if (name == "camera")
    return ResourceDomain::Camera;
  if (name == "guard")
    return ResourceDomain::Guard;
  if (name == "productivity")
    return ResourceDomain::Productivity;
  if (name == "identity")
    return ResourceDomain::Identity;
  if (name == "settings")
    return ResourceDomain::Settings;
  if (name == "activity")
    return ResourceDomain::Activity;
  return std::nullopt;
}

inline std::string resourceScopeToString(ResourceScope scope)
{
  switch (scope) {
    case ResourceScope::Device:
      return "device";
    case ResourceScope::Zones:
      return "zones";
    case ResourceScope::Status:
      return "status";
    case ResourceScope::Overview:
      return "overview";
    case ResourceScope::Environments:
      return "environments";
    case ResourceScope::Incidents:
      return "incidents";
    case ResourceScope::Episodes:
      return "episodes";
    case ResourceScope::ExpectedVisits:
      return "expected_visits";
    case ResourceScope::Responses:
      return "responses";
    case ResourceScope::SafetyPin:
      return "safety_pin";
    case ResourceScope::Duress:
      return "duress";
    case ResourceScope::Projects:
      return "projects";
    case ResourceScope::Calendar:
      return "calendar";
    case ResourceScope::PrivacyHousehold:
      return "privacy_household";
    case ResourceScope::PrivacyChoice:
      return "privacy_choice";
    case ResourceScope::Visitors:
      return "visitors";
    case ResourceScope::Values:
      return "values";
    case ResourceScope::Voiceprints:
      return "voiceprints";
    case ResourceScope::Presence:
      return "presence";
    case ResourceScope::Entries:
      return "entries";
  }
  return "device";
}

inline std::optional<ResourceScope> resourceScopeFromString(std::string_view name)
{
  if (name == "device")
    return ResourceScope::Device;
  if (name == "zones")
    return ResourceScope::Zones;
  if (name == "status")
    return ResourceScope::Status;
  if (name == "overview")
    return ResourceScope::Overview;
  if (name == "environments")
    return ResourceScope::Environments;
  if (name == "incidents")
    return ResourceScope::Incidents;
  if (name == "episodes")
    return ResourceScope::Episodes;
  if (name == "expected_visits")
    return ResourceScope::ExpectedVisits;
  if (name == "responses")
    return ResourceScope::Responses;
  if (name == "safety_pin")
    return ResourceScope::SafetyPin;
  if (name == "duress")
    return ResourceScope::Duress;
  if (name == "projects")
    return ResourceScope::Projects;
  if (name == "calendar")
    return ResourceScope::Calendar;
  if (name == "privacy_household")
    return ResourceScope::PrivacyHousehold;
  if (name == "privacy_choice")
    return ResourceScope::PrivacyChoice;
  if (name == "visitors")
    return ResourceScope::Visitors;
  if (name == "values")
    return ResourceScope::Values;
  if (name == "voiceprints")
    return ResourceScope::Voiceprints;
  if (name == "presence")
    return ResourceScope::Presence;
  if (name == "entries")
    return ResourceScope::Entries;
  return std::nullopt;
}

inline const ResourceRoute* resourceRouteOf(ResourceDomain domain,
                                            ResourceScope scope)
{
  const auto found = std::ranges::find_if(
      kResourceRoutes, [domain, scope](const ResourceRoute& route) {
        return route.domain == domain && route.scope == scope;
      });
  return found == kResourceRoutes.end() ? nullptr : &*found;
}

namespace resource_change
{
inline Json::Value payload(ResourceDomain domain, ResourceScope scope,
                           const std::vector<int64_t>& ids, int64_t user = 0)
{
  Json::Value body(Json::objectValue);
  body[sync_change::kKindField] = sync_change::kKindResource;
  body[sync_change::kDomainField] = resourceDomainToString(domain);
  body[sync_change::kScopeField] = resourceScopeToString(scope);
  if (user > 0)
    body[sync_change::kUserField] = static_cast<Json::Int64>(user);
  if (!ids.empty()) {
    Json::Value list(Json::arrayValue);
    for (const auto id : ids)
      list.append(static_cast<Json::Int64>(id));
    body[sync_change::kIdsField] = std::move(list);
  }
  return body;
}
}
