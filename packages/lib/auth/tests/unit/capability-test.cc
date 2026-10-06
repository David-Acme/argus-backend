#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/capability.hxx>
#include <auth/role-access.hxx>
#include <auth/user-role.hxx>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::array kKnownRoles = {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest};

ModuleFlag flag(std::string id, bool enabled, std::vector<std::string> roles)
{
  return {.id = std::move(id),
          .enabled = enabled,
          .lifecycle = enabled ? "active" : "disabled",
          .dataPurgedAt = 0,
          .roles = std::move(roles),
          .name = {},
          .summary = {},
          .intro = {}};
}

ModuleSnapshot snapshotWith(bool surveillance, bool productivity)
{
  return ModuleSnapshot({flag("core", true, {}),
                         flag("surveillance", surveillance, {"guard"}),
                         flag("productivity", productivity, {})});
}

const std::array kStates = {
    snapshotWith(true, true),
    snapshotWith(false, true),
    snapshotWith(true, false),
    snapshotWith(false, false),
};

bool has(const std::vector<std::string_view>& list, std::string_view id)
{
  return std::ranges::find(list, id) != list.end();
}

struct RouteProbe
{
  std::string_view capability;
  drogon::HttpMethod method;
  std::string_view path;
};

struct TableProbe
{
  std::string_view capability;
  TableName table;
  RolePermission perm;
};

constexpr std::array kRouteProbes = std::to_array<RouteProbe>({
    {"sessions.manage", drogon::Get, "/auth/sessions"},
    {"privacy.own", drogon::Get, "/privacy/me"},
    {"notifications.register", drogon::Post, "/notification-token"},
    {"calls.join", drogon::Post, "/rtc/token"},
    {"heartbeat.read", drogon::Get, "/sync/heartbeat"},
    {"modules.read", drogon::Get, "/modules"},
    {"safety.panic", drogon::Post, "/guard/panic"},
    {"users.manage", drogon::Patch, "/user/1"},
    {"invitations.manage", drogon::Post, "/invitation"},
    {"privacy.household", drogon::Patch, "/privacy/household"},
    {"settings.manage", drogon::Patch, "/settings/llm"},
    {"modules.manage", drogon::Post, "/modules/surveillance/install"},
    {"activity.read", drogon::Get, "/sync/activity"},
    {"camera.view", drogon::Get, "/camera/overview"},
    {"camera.view", drogon::Post, "/camera/1/webrtc"},
    {"camera.talk", drogon::Post, "/camera/1/talk"},
    {"camera.manage", drogon::Post, "/camera"},
    {"zones.read", drogon::Get, "/zone"},
    {"zones.write", drogon::Post, "/zone"},
    {"guard.read", drogon::Get, "/guard/environments"},
    {"guard.read", drogon::Get, "/guard/incidents"},
    {"guard.mode.set", drogon::Post, "/guard/mode"},
    {"guard.guests.write", drogon::Post, "/guard/expected-guests"},
    {"guard.admin", drogon::Post, "/guard/environments"},
    {"response.duty", drogon::Post, "/guard/environments/1/duty"},
    {"safety.read", drogon::Get, "/guard/safety"},
    {"safety.respond", drogon::Get, "/notification/responses"},
    {"safety.respond", drogon::Get, "/notification/responses/1"},
    {"safety.respond", drogon::Patch, "/notification/responses/1"},
    {"safety.duress", drogon::Put, "/guard/safety/pin"},
    {"visitors.read", drogon::Get, "/visitor"},
    {"visitors.manage", drogon::Delete, "/visitor/1"},
    {"presence.read", drogon::Get, "/guard/presence"},
    {"agenda.read", drogon::Get, "/calendar-event"},
    {"agenda.write", drogon::Post, "/calendar-event"},
    {"projects.read", drogon::Get, "/project"},
    {"projects.write", drogon::Post, "/project"},
    {"people.read", drogon::Get, "/person"},
    {"people.write", drogon::Post, "/person"},
    {"reminders.read", drogon::Get, "/reminder"},
    {"reminders.write", drogon::Post, "/reminder"},
    {"reminders.write", drogon::Patch, "/reminder/1"},
    {"reminders.write", drogon::Delete, "/reminder/1"},
});

constexpr std::array kTableProbes = std::to_array<TableProbe>({
    {"profile.read", TableName::User, RolePermission::Read},
    {"notifications.read", TableName::Notification, RolePermission::Read},
    {"events.read", TableName::Event, RolePermission::Read},
    {"memory.manage", TableName::Memory, RolePermission::Create},
    {"reminders.read", TableName::Reminder, RolePermission::Read},
    {"reminders.read", TableName::ReminderDetail, RolePermission::Read},
    {"reminders.write", TableName::Reminder, RolePermission::Update},
    {"reminders.write", TableName::ReminderDetail, RolePermission::Create},
});

bool tableAllowed(UserRole role, const TableProbe& probe, const ModuleSnapshot& modules)
{
  return role_access::moduleTableActive(probe.table, modules) &&
         role_access::hasAccess({.role = role,
                                 .table = probe.table,
                                 .perm = probe.perm,
                                 .roleActive = modules.roleActive(role)});
}

std::vector<std::string_view> baselineIds()
{
  std::vector<std::string_view> ids;
  for (const auto& spec : role_access::kCapabilities) {
    if ((spec.roles & role_access::kBaselineBit) != 0)
      ids.push_back(spec.id);
  }
  return ids;
}
}

TEST_CASE("every capability id is unique, dotted and lowercase")
{
  std::vector<std::string_view> seen;
  for (const auto& spec : role_access::kCapabilities) {
    CAPTURE(spec.id);
    CHECK(std::ranges::find(seen, spec.id) == seen.end());
    seen.push_back(spec.id);
    CHECK(spec.id.find('.') != std::string_view::npos);
    CHECK(std::ranges::all_of(spec.id, [](char c) {
      return (c >= 'a' && c <= 'z') || c == '.';
    }));
    CHECK((spec.module == kCoreModule || spec.module == role_access::kSurveillanceModule ||
           spec.module == role_access::kProductivityModule));
  }
}

TEST_CASE("the Owner holds every capability but the request for a module it can simply enable")
{
  const auto modules = snapshotWith(true, true);
  const auto owner = role_access::capabilitiesFor({.role = UserRole::Owner, .modules = modules});
  for (const auto& spec : role_access::kCapabilities) {
    CAPTURE(spec.id);
    CHECK(has(owner, spec.id) == (spec.id != "modules.request"));
  }
}

TEST_CASE("a capability needs both the role and its module")
{
  const auto off = snapshotWith(false, false);
  const auto on = snapshotWith(true, true);
  CHECK(role_access::hasCapability({.role = UserRole::Resident, .modules = on, .capability = "camera.view"}));
  CHECK_FALSE(role_access::hasCapability({.role = UserRole::Resident, .modules = off, .capability = "camera.view"}));
  CHECK(role_access::hasCapability({.role = UserRole::Resident, .modules = on, .capability = "agenda.write"}));
  CHECK_FALSE(role_access::hasCapability({.role = UserRole::Resident, .modules = off, .capability = "agenda.write"}));
  CHECK_FALSE(role_access::hasCapability({.role = UserRole::Guest, .modules = on, .capability = "agenda.read"}));
  CHECK_FALSE(role_access::hasCapability({.role = UserRole::Guest, .modules = on, .capability = "not.a.capability"}));
  CHECK(role_access::hasCapability({.role = UserRole::Resident, .modules = off, .capability = "reminders.write"}));
  CHECK(role_access::hasCapability({.role = UserRole::Resident, .modules = off, .capability = "people.read"}));
}

TEST_CASE("a role whose module is off keeps the baseline and nothing else")
{
  const auto off = snapshotWith(false, true);
  CHECK_FALSE(off.roleActive(UserRole::Guard));
  CHECK(off.roleActive(UserRole::Resident));
  CHECK(off.roleActive(UserRole::Guest));
  CHECK(off.roleActive(UserRole::Owner));

  auto guard = role_access::capabilitiesFor({.role = UserRole::Guard, .modules = off});
  auto expected = baselineIds();
  std::ranges::sort(guard);
  std::ranges::sort(expected);
  CHECK(guard == expected);
  CHECK(has(guard, "safety.panic"));
  CHECK(has(guard, "reminders.read"));
  CHECK(has(guard, "reminders.write"));
  CHECK_FALSE(has(guard, "directory.read"));
  CHECK_FALSE(has(guard, "people.read"));
  CHECK_FALSE(has(guard, "assistant.voice"));
  CHECK_FALSE(has(guard, "camera.view"));

  const auto on = snapshotWith(true, true);
  CHECK(has(role_access::capabilitiesFor({.role = UserRole::Guard, .modules = on}), "directory.read"));
}

TEST_CASE("a role this build does not know holds nothing, active or not")
{
  for (const auto& modules : kStates) {
    CHECK(role_access::capabilitiesFor({.role = UserRole::Unknown, .modules = modules}).empty());
    CHECK_FALSE(modules.roleActive(UserRole::Unknown));
    CHECK_FALSE(role_access::hasCapability(
        {.role = UserRole::Unknown, .modules = modules, .capability = "safety.panic"}));
  }
}

TEST_CASE("capabilities and the route and table rules never drift apart")
{
  for (const auto& modules : kStates) {
    for (const auto role : kKnownRoles) {
      for (const auto& probe : kRouteProbes) {
        CAPTURE(userRoleToString(role));
        CAPTURE(probe.capability);
        CAPTURE(probe.path);
        const bool allowed = role_access::routeVerdict({.role = role,
                                                        .path = probe.path,
                                                        .method = probe.method,
                                                        .modules = modules}) ==
                             role_access::RouteVerdict::Allowed;
        CHECK(role_access::hasCapability(
                  {.role = role, .modules = modules, .capability = probe.capability}) == allowed);
      }
      for (const auto& probe : kTableProbes) {
        CAPTURE(userRoleToString(role));
        CAPTURE(probe.capability);
        CAPTURE(tableNameToString(probe.table));
        CHECK(role_access::hasCapability(
                  {.role = role, .modules = modules, .capability = probe.capability}) ==
              tableAllowed(role, probe, modules));
      }
      CAPTURE(userRoleToString(role));
      CHECK(role_access::hasCapability({.role = role, .modules = modules, .capability = "directory.read"}) ==
            role_access::readsUserDirectory(role, modules.roleActive(role)));
    }
  }
}

TEST_CASE("an app action the assistant may trigger follows the capability, so a module that is off refuses it")
{
  for (const auto& modules : kStates) {
    for (const auto role : kKnownRoles) {
      for (const auto action : {role_access::AppAction::ShowCamera, role_access::AppAction::OpenScreen,
                                role_access::AppAction::SetGuardMode}) {
        CAPTURE(userRoleToString(role));
        CAPTURE(static_cast<int>(action));
        const bool allowed = role_access::hasAppAction({.role = role, .action = action, .modules = modules});
        const bool tableAllows = role_access::hasAppAction(role, action);
        const bool moduleOn = action == role_access::AppAction::OpenScreen || modules.enabled("surveillance");
        const bool baseline = action == role_access::AppAction::OpenScreen;
        CHECK(allowed == (tableAllows && moduleOn && (modules.roleActive(role) || baseline)));
      }
    }
  }
  const auto off = snapshotWith(false, true);
  CHECK_FALSE(role_access::hasAppAction(
      {.role = UserRole::Owner, .action = role_access::AppAction::ShowCamera, .modules = off}));
  CHECK_FALSE(role_access::hasAppAction(
      {.role = UserRole::Unknown, .action = role_access::AppAction::OpenScreen, .modules = snapshotWith(true, true)}));
}

TEST_CASE("the request for a module is held by every role but the Owner, an inactive one included")
{
  const auto off = snapshotWith(false, true);
  for (const auto role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(userRoleToString(role));
    CHECK(role_access::hasCapability({.role = role, .modules = off, .capability = "modules.request"}));
    CHECK(role_access::routeVerdict({.role = role,
                                     .path = "/modules/surveillance/request",
                                     .method = drogon::Post,
                                     .modules = off}) == role_access::RouteVerdict::Allowed);
  }
  CHECK_FALSE(role_access::hasCapability({.role = UserRole::Owner, .modules = off, .capability = "modules.request"}));
}

TEST_CASE("reminders are core and own-row for every role, the Owner included")
{
  for (const auto& modules : kStates) {
    for (const auto role : kKnownRoles) {
      CAPTURE(userRoleToString(role));
      CHECK(role_access::hasCapability({.role = role, .modules = modules, .capability = "reminders.read"}));
      CHECK(role_access::hasCapability({.role = role, .modules = modules, .capability = "reminders.write"}));
      CHECK_FALSE(role_access::moduleOfRoute("/reminder", drogon::Get).has_value());
      CHECK_FALSE(role_access::moduleOfRoute("/reminder/4", drogon::Patch).has_value());
      CHECK_FALSE(role_access::moduleOfRoute("/reminder-detail/4", drogon::Delete).has_value());
      CHECK(role_access::tableReadable(role, TableName::Reminder, modules));
      CHECK(role_access::tableReadable(role, TableName::ReminderDetail, modules));
      const auto rooms = role_access::moduleTables(role, modules);
      CHECK(std::ranges::find(rooms, TableName::Reminder) == rooms.end());
      CHECK(std::ranges::find(rooms, TableName::ReminderDetail) == rooms.end());
    }
  }
  for (const auto& spec : role_access::kCapabilities) {
    CHECK(spec.id != "reminders.read.all");
    CHECK(spec.id != "reminders.write.all");
  }
  for (const auto role : {UserRole::Guard, UserRole::Guest}) {
    const auto& table = role_access::kTableAccess.at(role);
    const auto& resident = role_access::kTableAccess.at(UserRole::Resident);
    CHECK(table.at(TableName::Reminder) == resident.at(TableName::Reminder));
    CHECK(table.at(TableName::ReminderDetail) == resident.at(TableName::ReminderDetail));
  }
}

TEST_CASE("the panic route is core: never module-gated, kept by an inactive role")
{
  for (const auto& modules : kStates) {
    for (const auto role : {UserRole::Resident, UserRole::Guard, UserRole::Guest, UserRole::Owner}) {
      CAPTURE(userRoleToString(role));
      CHECK(role_access::routeVerdict({.role = role,
                                       .path = "/guard/panic",
                                       .method = drogon::Post,
                                       .modules = modules}) == role_access::RouteVerdict::Allowed);
      CHECK(role_access::routeVerdict({.role = role,
                                       .path = "/GUARD/Panic",
                                       .method = drogon::Post,
                                       .modules = modules}) == role_access::RouteVerdict::Allowed);
    }
  }
  const auto off = snapshotWith(false, true);
  for (const auto role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(userRoleToString(role));
    CHECK(role_access::routeVerdict({.role = role,
                                     .path = "/guard/safety",
                                     .method = drogon::Get,
                                     .modules = off}) == role_access::RouteVerdict::Allowed);
    CHECK(role_access::routeVerdict({.role = role,
                                     .path = "/guard/safety/pin",
                                     .method = drogon::Put,
                                     .modules = off}) == role_access::RouteVerdict::ModuleDisabled);
    CHECK(role_access::routeVerdict({.role = role,
                                     .path = "/guard/mode",
                                     .method = drogon::Post,
                                     .modules = off}) == role_access::RouteVerdict::ModuleDisabled);
  }
  CHECK(role_access::routeVerdict({.role = UserRole::Owner,
                                   .path = "/guard/safety",
                                   .method = drogon::Patch,
                                   .modules = off}) == role_access::RouteVerdict::ModuleDisabled);
  CHECK(role_access::routeVerdict({.role = UserRole::Owner,
                                   .path = "/guard/safety/pin",
                                   .method = drogon::Delete,
                                   .modules = off}) == role_access::RouteVerdict::ModuleDisabled);
  CHECK(role_access::routeVerdict({.role = UserRole::Guest,
                                   .path = "/guard/panic",
                                   .method = drogon::Get,
                                   .modules = off}) == role_access::RouteVerdict::ModuleDisabled);
}

TEST_CASE("an inactive role is refused ROLE_INACTIVE on core routes beyond the baseline")
{
  const auto off = snapshotWith(false, true);
  const auto guard = [&](std::string_view path, drogon::HttpMethod method) {
    return role_access::routeVerdict({.role = UserRole::Guard, .path = path, .method = method, .modules = off});
  };
  CHECK(guard("/person", drogon::Get) == role_access::RouteVerdict::RoleInactive);
  CHECK(guard("/memory", drogon::Get) == role_access::RouteVerdict::Denied);
  CHECK(guard("/auth/sessions", drogon::Get) == role_access::RouteVerdict::Allowed);
  CHECK(guard("/privacy/me", drogon::Put) == role_access::RouteVerdict::Allowed);
  CHECK(guard("/notification/read", drogon::Patch) == role_access::RouteVerdict::Allowed);
  CHECK(guard("/reminder/3", drogon::Patch) == role_access::RouteVerdict::Allowed);
  CHECK(guard("/modules", drogon::Get) == role_access::RouteVerdict::Allowed);
  CHECK(guard("/camera/1", drogon::Get) == role_access::RouteVerdict::ModuleDisabled);
  CHECK(guard("/visitor", drogon::Get) == role_access::RouteVerdict::ModuleDisabled);
  CHECK(role_access::readsUserDirectory(UserRole::Guard, true));
  CHECK_FALSE(role_access::readsUserDirectory(UserRole::Guard, false));
  const auto tables = role_access::moduleTables(UserRole::Guard, off);
  for (const auto table : tables)
    CHECK(role_access::isBaselineTable(table));
}

TEST_CASE("sync pulls and rooms skip the tables of an inactive module and keep the core ones")
{
  const auto noProductivity = snapshotWith(true, false);
  CHECK_FALSE(role_access::tableReadable(UserRole::Resident, TableName::Project, noProductivity));
  CHECK_FALSE(role_access::tableReadable(UserRole::Owner, TableName::CalendarEvent, noProductivity));
  CHECK(role_access::tableReadable(UserRole::Resident, TableName::Camera, noProductivity));
  CHECK(role_access::tableReadable(UserRole::Resident, TableName::Reminder, noProductivity));
  CHECK(role_access::tableReadable(UserRole::Owner, TableName::Notification, noProductivity));

  const auto noSurveillance = snapshotWith(false, true);
  for (const auto table : {TableName::Camera, TableName::CameraStream, TableName::Zone, TableName::Event})
    CHECK_FALSE(role_access::tableReadable(UserRole::Resident, table, noSurveillance));
  const auto residentTables = role_access::moduleTables(UserRole::Resident, noSurveillance);
  CHECK(std::ranges::find(residentTables, TableName::Camera) == residentTables.end());
  CHECK(std::ranges::find(residentTables, TableName::Project) != residentTables.end());
  CHECK(std::ranges::find(residentTables, TableName::Person) != residentTables.end());
}

TEST_CASE("every table of a gated module is gated by the same module as its routes")
{
  for (const auto& entry : role_access::kTableModules) {
    CAPTURE(tableNameToString(entry.table));
    const auto module = role_access::moduleOfTable(entry.table);
    REQUIRE(module.has_value());
    CHECK(*module == entry.module);
  }
  for (const auto& route : role_access::kModuleRoutes) {
    const std::string path = "/" + std::string(route.segment) + "/1";
    CHECK(role_access::moduleOfPath(path) == route.module);
  }
  CHECK_FALSE(role_access::moduleOfTable(TableName::Reminder).has_value());
  CHECK_FALSE(role_access::moduleOfTable(TableName::Person).has_value());
}

TEST_CASE("the baseline keeps reminders, panic, the safety state and the responder routes for every role in every module state")
{
  constexpr std::array kBaseline = {"reminders.read", "reminders.write", "safety.panic", "safety.read",
                                    "safety.respond", "calls.join", "notifications.read", "profile.read"};
  for (const auto& modules : kStates) {
    for (const auto role : kKnownRoles) {
      CAPTURE(userRoleToString(role));
      const auto held = role_access::capabilitiesFor({.role = role, .modules = modules});
      for (const auto* id : kBaseline) {
        CAPTURE(id);
        CHECK(has(held, id));
      }
    }
    CHECK(role_access::capabilitiesFor({.role = UserRole::Unknown, .modules = modules}).empty());
  }
  const auto everything = snapshotWith(false, false);
  CHECK_FALSE(everything.roleActive(UserRole::Guard));
  const auto guard = role_access::capabilitiesFor({.role = UserRole::Guard, .modules = everything});
  for (const auto* id : kBaseline)
    CHECK(has(guard, id));
}

TEST_CASE("modules.request goes to every role but the Owner, active or inactive, in every module state")
{
  for (const auto& modules : kStates) {
    for (const auto role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
      CAPTURE(userRoleToString(role));
      CHECK(has(role_access::capabilitiesFor({.role = role, .modules = modules}), "modules.request"));
    }
    CHECK_FALSE(has(role_access::capabilitiesFor({.role = UserRole::Owner, .modules = modules}),
                    "modules.request"));
    CHECK_FALSE(has(role_access::capabilitiesFor({.role = UserRole::Unknown, .modules = modules}),
                    "modules.request"));
  }
}

TEST_CASE("a raised alert stays answerable with surveillance off: the safety state, the responder routes and the call")
{
  struct Step
  {
    drogon::HttpMethod method;
    std::string_view path;
  };
  constexpr std::array steps = {Step{drogon::Get, "/guard/safety"},
                                Step{drogon::Get, "/notification/responses"},
                                Step{drogon::Get, "/notification/responses/9"},
                                Step{drogon::Patch, "/notification/responses/9"},
                                Step{drogon::Patch, "/notification/ack"},
                                Step{drogon::Patch, "/notification/read"},
                                Step{drogon::Post, "/rtc/token"},
                                Step{drogon::Post, "/guard/panic"}};
  for (const auto& modules : kStates) {
    for (const auto role : kKnownRoles) {
      for (const auto& step : steps) {
        CAPTURE(userRoleToString(role));
        CAPTURE(step.path);
        CHECK(role_access::routeVerdict({.role = role, .path = step.path, .method = step.method, .modules = modules}) ==
              role_access::RouteVerdict::Allowed);
      }
    }
    for (const auto& step : steps) {
      CAPTURE(step.path);
      CHECK(role_access::routeVerdict({.role = UserRole::Unknown,
                                       .path = step.path,
                                       .method = step.method,
                                       .modules = modules}) == role_access::RouteVerdict::Denied);
    }
  }
  const auto off = snapshotWith(false, true);
  for (const auto& step : steps)
    CHECK(role_access::routeVerdict({.role = UserRole::Guard, .path = step.path, .method = step.method, .modules = off}) ==
          role_access::RouteVerdict::Allowed);
  CHECK(role_access::routeVerdict({.role = UserRole::Guard,
                                   .path = "/guard/episodes",
                                   .method = drogon::Get,
                                   .modules = off}) == role_access::RouteVerdict::ModuleDisabled);
  CHECK(role_access::routeVerdict({.role = UserRole::Guard,
                                   .path = "/guard/environments/1/duty",
                                   .method = drogon::Post,
                                   .modules = off}) == role_access::RouteVerdict::ModuleDisabled);
}
