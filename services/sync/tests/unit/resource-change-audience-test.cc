#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/module-snapshot.hxx>
#include <auth/role-access.hxx>
#include <auth/user-role.hxx>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <shared/services/context/user-context.hxx>
#include <shared/services/room/room-manager.hxx>
#include <sync/resource-change.hxx>
#include <sync/sync-change.hxx>
#include <sync/sync-limits.hxx>
#include <text/json-util.hxx>

#include <algorithm>
#include <string>
#include <vector>

namespace
{
ModuleSnapshot everyModuleEnabled()
{
  return ModuleSnapshot{ModuleFlags{
      {.id = "surveillance", .enabled = true, .roles = {"guard"}},
      {.id = "productivity", .enabled = true, .roles = {}},
  }};
}

ModuleSnapshot surveillanceDisabled()
{
  return ModuleSnapshot{ModuleFlags{
      {.id = "surveillance", .enabled = false, .roles = {"guard"}},
      {.id = "productivity", .enabled = true, .roles = {}},
  }};
}

std::string roomNames(const std::vector<RoomId>& rooms)
{
  std::string names;
  for (const UserRole role :
       {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    if (std::ranges::find(rooms, roleRoom(role)) == rooms.end())
      continue;
    if (!names.empty())
      names += ',';
    names += userRoleToString(role);
  }
  return names;
}

std::string audienceTable(const ModuleSnapshot& modules)
{
  std::string table;
  for (const auto& route : kResourceRoutes) {
    table += resourceDomainToString(route.domain);
    table += '/';
    table += resourceScopeToString(route.scope);
    table += " ->";
    const std::string names = roomNames(sync_fan_out::resourceAudience(route, modules));
    if (!names.empty()) {
      table += ' ';
      table += names;
    }
    table += '\n';
  }
  return table;
}
}

TEST_CASE("the resource audience table with every module enabled")
{
  for (const auto& route : kResourceRoutes)
    CHECK_FALSE(sync_fan_out::resourceAudience(route, everyModuleEnabled()).empty());

  CHECK(audienceTable(everyModuleEnabled()) ==
        R"(camera/device -> owner,resident,guard,guest
camera/zones -> owner,resident,guard
camera/status -> owner,resident,guard,guest
camera/overview -> owner,resident,guard,guest
guard/environments -> owner,resident,guard
guard/incidents -> owner,resident,guard
guard/episodes -> owner,resident,guard
guard/expected_visits -> owner,resident,guard
guard/responses -> owner,resident,guard
guard/safety_pin -> owner,resident,guard,guest
guard/duress -> owner,resident,guard,guest
productivity/projects -> owner,resident
productivity/calendar -> owner,resident
identity/privacy_household -> owner
identity/privacy_choice -> owner,resident,guard,guest
identity/visitors -> owner,guard
settings/values -> owner
identity/voiceprints -> owner
guard/presence -> owner
activity/entries -> owner
)");
}

TEST_CASE("the resource audience table with surveillance disabled")
{
  CHECK(audienceTable(surveillanceDisabled()) ==
        R"(camera/device ->
camera/zones ->
camera/status ->
camera/overview ->
guard/environments ->
guard/incidents ->
guard/episodes ->
guard/expected_visits ->
guard/responses ->
guard/safety_pin -> owner,resident,guard,guest
guard/duress -> owner,resident,guard,guest
productivity/projects -> owner,resident
productivity/calendar -> owner,resident
identity/privacy_household -> owner
identity/privacy_choice -> owner,resident,guard,guest
identity/visitors ->
settings/values -> owner
identity/voiceprints -> owner
guard/presence ->
activity/entries -> owner
)");
}

TEST_CASE("a resource payload parses only when the pair is registered")
{
  const auto parse = [](const char* text) {
    return sync_fan_out::parseResourcePayload(json_util::fromString(text));
  };

  const auto plain = parse(R"({"domain":"guard","scope":"environments"})");
  if (!plain) {
    FAIL("expected a value in plain");
    return;
  }
  CHECK(plain->route == resourceRouteOf(ResourceDomain::Guard, ResourceScope::Environments));
  CHECK(plain->info["domain"] == "guard");
  CHECK(plain->info["scope"] == "environments");
  CHECK_FALSE(plain->info.isMember(sync_change::kIdsField));
  CHECK_FALSE(plain->user.has_value());

  const auto withIds = parse(R"({"domain":"camera","scope":"device","ids":[2,5]})");
  if (!withIds) {
    FAIL("expected a value in withIds");
    return;
  }
  CHECK(withIds->info[sync_change::kIdsField].size() == 2);

  const auto targeted = parse(R"({"domain":"guard","scope":"safety_pin","user":42})");
  if (!targeted) {
    FAIL("expected a value in targeted");
    return;
  }
  CHECK(targeted->user == 42);

  CHECK_FALSE(parse(R"(["not an object"])").has_value());
  CHECK_FALSE(parse(R"({"scope":"environments"})").has_value());
  CHECK_FALSE(parse(R"({"domain":"guard"})").has_value());
  CHECK_FALSE(parse(R"({"domain":"gate","scope":"environments"})").has_value());
  CHECK_FALSE(parse(R"({"domain":"guard","scope":"guests"})").has_value());
  CHECK_FALSE(parse(R"({"domain":"guard","scope":"environments","ids":5})").has_value());
  CHECK_FALSE(parse(R"({"domain":"guard","scope":"environments","ids":[0]})").has_value());
  CHECK_FALSE(parse(R"({"domain":"guard","scope":"environments","ids":["a"]})").has_value());
  CHECK_FALSE(parse(R"({"domain":"guard","scope":"environments","user":0})").has_value());
  CHECK_FALSE(parse(R"({"domain":"guard","scope":"environments","user":-1})").has_value());
}

TEST_CASE("the ids of a resource payload stay under the scoped-pull bound")
{
  std::string over = R"({"domain":"camera","scope":"device","ids":[)";
  for (std::size_t index = 0; index <= SyncLimits::kMaxScopeIds; ++index) {
    if (index > 0)
      over += ',';
    over += std::to_string(index + 1);
  }
  over += "]}";
  CHECK_FALSE(sync_fan_out::parseResourcePayload(json_util::fromString(over)).has_value());
}

TEST_CASE("a user-targeted resource frame answers for the role this process knows")
{
  const ResourceRoute* pin =
      resourceRouteOf(ResourceDomain::Guard, ResourceScope::SafetyPin);
  const ResourceRoute* household = resourceRouteOf(
      ResourceDomain::Identity, ResourceScope::PrivacyHousehold);
  REQUIRE(pin != nullptr);
  REQUIRE(household != nullptr);
  const ModuleSnapshot modules = everyModuleEnabled();

  const auto unknown = sync_fan_out::resourceDelivery(*pin, modules, 771);
  CHECK(unknown.rooms.empty());
  CHECK_FALSE(unknown.refused);

  userContext().remember(772, UserRole::Guard);
  const auto allowed = sync_fan_out::resourceDelivery(*pin, modules, 772);
  CHECK(allowed.rooms == std::vector<RoomId>{userRoom(772)});
  CHECK_FALSE(allowed.refused);

  userContext().remember(773, UserRole::Guard);
  const auto denied = sync_fan_out::resourceDelivery(*household, modules, 773);
  CHECK(denied.rooms.empty());
  CHECK(denied.refused);
}
