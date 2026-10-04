#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <drogon/HttpTypes.h>
#include <auth/role-access.hxx>
#include <sync/table-name.hxx>
#include <auth/user-role.hxx>
#include <vector>

TEST_CASE("Owner has every permission on every table")
{
    for (int table = static_cast<int>(TableName::User);
         table <= static_cast<int>(kLastTableName); ++table) {
        const auto name = static_cast<TableName>(table);
        CHECK(role_access::hasAccess({.role = UserRole::Owner, .table = name,
                                      .perm = RolePermission::Read}));
        CHECK(role_access::hasAccess({.role = UserRole::Owner, .table = name,
                                      .perm = RolePermission::Create}));
        CHECK(role_access::hasAccess({.role = UserRole::Owner, .table = name,
                                      .perm = RolePermission::Update}));
        CHECK(role_access::hasAccess({.role = UserRole::Owner, .table = name,
                                      .perm = RolePermission::Delete}));
    }
}

TEST_CASE("Resident permissions follow the kTableAccess map")
{
    CHECK(role_access::hasAccess({.role = UserRole::Resident,
                                  .table = TableName::Camera,
                                  .perm = RolePermission::Read}));
    CHECK(role_access::hasAccess({.role = UserRole::Resident,
                                  .table = TableName::Camera,
                                  .perm = RolePermission::Create}));
    CHECK(role_access::hasAccess({.role = UserRole::Resident,
                                  .table = TableName::Camera,
                                  .perm = RolePermission::Update}));
    CHECK(role_access::hasAccess({.role = UserRole::Resident,
                                  .table = TableName::Camera,
                                  .perm = RolePermission::Delete}));

    CHECK(role_access::hasAccess({.role = UserRole::Resident,
                                  .table = TableName::Zone,
                                  .perm = RolePermission::Delete}));
    CHECK(role_access::hasAccess({.role = UserRole::Resident,
                                  .table = TableName::Memory,
                                  .perm = RolePermission::Create}));

    CHECK(role_access::hasAccess({.role = UserRole::Resident,
                                  .table = TableName::User,
                                  .perm = RolePermission::Read}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Resident,
                                        .table = TableName::User,
                                        .perm = RolePermission::Create}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Resident,
                                        .table = TableName::User,
                                        .perm = RolePermission::Update}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Resident,
                                        .table = TableName::User,
                                        .perm = RolePermission::Delete}));

    CHECK(role_access::hasAccess({.role = UserRole::Resident,
                                  .table = TableName::Notification,
                                  .perm = RolePermission::Read}));
    CHECK(role_access::hasAccess({.role = UserRole::Resident,
                                  .table = TableName::Notification,
                                  .perm = RolePermission::Update}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Resident,
                                        .table = TableName::Notification,
                                        .perm = RolePermission::Delete}));

    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Resident,
                                        .table = TableName::UserInvitation,
                                        .perm = RolePermission::Read}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Resident,
                                        .table = TableName::NotificationToken,
                                        .perm = RolePermission::Read}));
}

TEST_CASE("Guard permissions follow the kTableAccess map")
{
    CHECK(role_access::hasAccess({.role = UserRole::Guard,
                                  .table = TableName::Camera,
                                  .perm = RolePermission::Read}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Guard,
                                        .table = TableName::Camera,
                                        .perm = RolePermission::Create}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Guard,
                                        .table = TableName::Camera,
                                        .perm = RolePermission::Delete}));

    CHECK(role_access::hasAccess({.role = UserRole::Guard,
                                  .table = TableName::Person,
                                  .perm = RolePermission::Read}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Guard,
                                        .table = TableName::Person,
                                        .perm = RolePermission::Update}));

    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Guard,
                                        .table = TableName::Reminder,
                                        .perm = RolePermission::Read}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Guard,
                                        .table = TableName::Memory,
                                        .perm = RolePermission::Read}));
}

TEST_CASE("Guest permissions follow the kTableAccess map")
{
    CHECK(role_access::hasAccess({.role = UserRole::Guest,
                                  .table = TableName::Camera,
                                  .perm = RolePermission::Read}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Guest,
                                        .table = TableName::Camera,
                                        .perm = RolePermission::Update}));

    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Guest,
                                        .table = TableName::Person,
                                        .perm = RolePermission::Read}));

    CHECK(role_access::hasAccess({.role = UserRole::Guest,
                                  .table = TableName::NotificationToken,
                                  .perm = RolePermission::Create}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Guest,
                                        .table = TableName::NotificationToken,
                                        .perm = RolePermission::Read}));
}

TEST_CASE("readableTables lists every table a role can read")
{
    const auto owner = role_access::readableTables(UserRole::Owner);
    CHECK(owner.size() == static_cast<size_t>(kLastTableName) + 1 - 3);
    for (const auto table : owner) {
        CHECK(table != TableName::RefreshToken);
        CHECK(table != TableName::FaceEmbedding);
        CHECK(table != TableName::PersonEvent);
    }

    const auto resident = role_access::readableTables(UserRole::Resident);
    CHECK(resident.size() == 18);

    const auto guard = role_access::readableTables(UserRole::Guard);
    CHECK(guard.size() == 9);

    const auto guest = role_access::readableTables(UserRole::Guest);
    CHECK(guest.size() == 5);
}

TEST_CASE("permissionForMethod maps HTTP methods to permissions")
{
    CHECK(role_access::permissionForMethod(drogon::Get) ==
          RolePermission::Read);
    CHECK(role_access::permissionForMethod(drogon::Post) ==
          RolePermission::Create);
    CHECK(role_access::permissionForMethod(drogon::Patch) ==
          RolePermission::Update);
    CHECK(role_access::permissionForMethod(drogon::Delete) ==
          RolePermission::Delete);
}

TEST_CASE("tableFromPath resolves the longest matching prefix")
{
    CHECK(role_access::tableFromPath("/camera") == TableName::Camera);
    CHECK(role_access::tableFromPath("/camera/1") == TableName::Camera);
    CHECK(role_access::tableFromPath("/camera-stream/7") ==
          TableName::CameraStream);
    CHECK(role_access::tableFromPath("/reminder-detail/3") ==
          TableName::ReminderDetail);
    CHECK(role_access::tableFromPath("/reminder/3") == TableName::Reminder);
    CHECK(role_access::tableFromPath("/calendar-event-share/9") ==
          TableName::CalendarEventShare);
    CHECK(role_access::tableFromPath("/calendar-event/9") ==
          TableName::CalendarEvent);
    CHECK(role_access::tableFromPath("/project-member") ==
          TableName::ProjectMember);
    CHECK(role_access::tableFromPath("/project") == TableName::Project);
    CHECK(role_access::tableFromPath("/invitation") ==
          TableName::UserInvitation);
    CHECK(role_access::tableFromPath("/user/42") == TableName::User);
    CHECK(role_access::tableFromPath("/notification-token") ==
          TableName::NotificationToken);
    CHECK(role_access::tableFromPath("/portrait-preview/token/content") ==
          TableName::User);
    CHECK_FALSE(role_access::tableFromPath("/unknown").has_value());
    CHECK_FALSE(role_access::tableFromPath("/sync").has_value());
}

TEST_CASE("hasHttpAccess allows the Owner everywhere")
{
    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Owner, .path = "/camera", .method = drogon::Delete}));
    CHECK(role_access::hasHttpAccess({.role = UserRole::Owner,
                                      .path = "/auth/logout",
                                      .method = drogon::Delete}));
    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Owner, .path = "/anything", .method = drogon::Get}));
}

TEST_CASE("hasHttpAccess enforces table permissions per role")
{
    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Resident, .path = "/camera/1", .method = drogon::Get}));
    CHECK(role_access::hasHttpAccess({.role = UserRole::Resident,
                                      .path = "/camera/1",
                                      .method = drogon::Delete}));
    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Resident, .path = "/zone", .method = drogon::Post}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Guest,
                                            .path = "/camera/1",
                                            .method = drogon::Delete}));
    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Guard, .path = "/camera/1", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess(
        {.role = UserRole::Guard, .path = "/camera/1", .method = drogon::Post}));
    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Guest, .path = "/camera", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess(
        {.role = UserRole::Guest, .path = "/reminder", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess(
        {.role = UserRole::Guest, .path = "/sync", .method = drogon::Get}));
}

TEST_CASE("hasHttpAccess applies kAuthAccess to /auth paths")
{
    CHECK(role_access::hasHttpAccess({.role = UserRole::Resident,
                                      .path = "/auth/login",
                                      .method = drogon::Post}));
    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Resident, .path = "/auth/me", .method = drogon::Get}));
    CHECK(role_access::hasHttpAccess({.role = UserRole::Resident,
                                      .path = "/auth/profile",
                                      .method = drogon::Patch}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Resident,
                                            .path = "/auth/logout",
                                            .method = drogon::Delete}));

    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Guard, .path = "/auth/me", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Guard,
                                            .path = "/auth/login",
                                            .method = drogon::Post}));

    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Guest, .path = "/auth/me", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Guest,
                                            .path = "/auth/logout",
                                            .method = drogon::Delete}));
}

TEST_CASE("guard calibration stays Owner-only while the Owner keeps every guard route")
{
  CHECK(role_access::hasHttpAccess(
      {.role = UserRole::Owner, .path = "/guard/mode", .method = drogon::Post}));
  CHECK(role_access::hasHttpAccess({.role = UserRole::Owner,
                                    .path = "/guard/expected-guests",
                                    .method = drogon::Delete}));
  CHECK_FALSE(role_access::hasHttpAccess(
      {.role = UserRole::Guard, .path = "/guard/decisions", .method = drogon::Get}));
  CHECK_FALSE(role_access::hasHttpAccess(
      {.role = UserRole::Guest, .path = "/guard/mode", .method = drogon::Post}));
}

TEST_CASE("only directory roles receive the user module stream")
{
  const auto contains = [](const std::vector<TableName>& tables,
                           TableName table) {
    return std::ranges::find(tables, table) != tables.end();
  };

  CHECK(role_access::readsUserDirectory(UserRole::Owner));
  CHECK(role_access::readsUserDirectory(UserRole::Guard));
  CHECK_FALSE(role_access::readsUserDirectory(UserRole::Resident));
  CHECK_FALSE(role_access::readsUserDirectory(UserRole::Guest));

  CHECK(contains(role_access::moduleTables(UserRole::Owner), TableName::User));
  CHECK(contains(role_access::moduleTables(UserRole::Guard), TableName::User));
  CHECK_FALSE(
      contains(role_access::moduleTables(UserRole::Resident), TableName::User));
  CHECK_FALSE(
      contains(role_access::moduleTables(UserRole::Guest), TableName::User));
  CHECK(contains(role_access::readableTables(UserRole::Resident),
                 TableName::User));
  CHECK(contains(role_access::moduleTables(UserRole::Resident),
                 TableName::Reminder));
}

TEST_CASE("hasHttpAccess applies kGuardAccess route by route")
{
    const auto allows = [](UserRole role, std::string_view path, drogon::HttpMethod method) {
        return role_access::hasHttpAccess({.role = role, .path = path, .method = method});
    };

    CHECK(allows(UserRole::Resident, "/guard/mode", drogon::Get));
    CHECK(allows(UserRole::Resident, "/guard/mode", drogon::Post));
    CHECK(allows(UserRole::Resident, "/guard/incidents", drogon::Get));
    CHECK(allows(UserRole::Resident, "/guard/expected-guests", drogon::Post));
    CHECK(allows(UserRole::Resident, "/guard/expected-guests", drogon::Delete));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/decisions", drogon::Get));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/decisions/summary", drogon::Get));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/decisions/7/feedback", drogon::Post));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/person/3/promote", drogon::Post));

    CHECK(allows(UserRole::Guard, "/guard/mode", drogon::Get));
    CHECK(allows(UserRole::Guard, "/guard/incidents", drogon::Get));
    CHECK(allows(UserRole::Guard, "/guard/expected-guests", drogon::Get));
    CHECK(allows(UserRole::Guard, "/guard/episodes", drogon::Get));
    CHECK(allows(UserRole::Guard, "/guard/site", drogon::Get));
    CHECK(allows(UserRole::Resident, "/guard/episodes", drogon::Get));
    CHECK(allows(UserRole::Resident, "/guard/site", drogon::Get));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/site", drogon::Patch));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/episodes/4", drogon::Get));
    CHECK_FALSE(allows(UserRole::Guard, "/guard/episodes/4/review", drogon::Post));
    CHECK_FALSE(allows(UserRole::Guest, "/guard/episodes", drogon::Get));
    CHECK_FALSE(allows(UserRole::Guard, "/guard/mode", drogon::Post));
    CHECK_FALSE(allows(UserRole::Guard, "/guard/expected-guests", drogon::Post));
    CHECK_FALSE(allows(UserRole::Guard, "/guard/decisions", drogon::Get));

    CHECK_FALSE(allows(UserRole::Guest, "/guard/mode", drogon::Get));
    CHECK_FALSE(allows(UserRole::Guest, "/guard/incidents", drogon::Get));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/mode/extra", drogon::Get));
    CHECK_FALSE(allows(UserRole::Resident, "/guardian", drogon::Get));

    CHECK(allows(UserRole::Owner, "/guard/decisions", drogon::Get));
}

TEST_CASE("every role lists and revokes its own sessions through kSessionAccess")
{
  const auto allows = [](UserRole role, std::string_view path, drogon::HttpMethod method) {
    return role_access::hasHttpAccess({.role = role, .path = path, .method = method});
  };

  for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(userRoleToString(role));
    CHECK(allows(role, "/auth/sessions", drogon::Get));
    CHECK(allows(role, "/auth/sessions", drogon::Delete));
    CHECK(allows(role, "/auth/sessions/0123456789abcdef0123456789abcdef", drogon::Delete));
  }

  CHECK_FALSE(allows(UserRole::Guard, "/auth/sessions/", drogon::Delete));
  CHECK_FALSE(allows(UserRole::Guard, "/auth/sessions/a/b", drogon::Delete));
  CHECK_FALSE(allows(UserRole::Guest, "/auth/sessions", drogon::Post));
  CHECK_FALSE(allows(UserRole::Guard, "/auth/sessionsx", drogon::Delete));
  CHECK_FALSE(allows(UserRole::Guest, "/auth/logout", drogon::Delete));
  CHECK(allows(UserRole::Guard, "/auth/me", drogon::Get));
}

TEST_CASE("only the owner reads and revokes another user's sessions")
{
  const auto allows = [](UserRole role, std::string_view path, drogon::HttpMethod method) {
    return role_access::hasHttpAccess({.role = role, .path = path, .method = method});
  };
  constexpr std::string_view kOne = "/auth/users/7/sessions/0123456789abcdef0123456789abcdef";

  CHECK(allows(UserRole::Owner, "/auth/users/sessions", drogon::Get));
  CHECK(allows(UserRole::Owner, "/auth/users/7/sessions", drogon::Get));
  CHECK(allows(UserRole::Owner, "/auth/users/7/sessions", drogon::Delete));
  CHECK(allows(UserRole::Owner, kOne, drogon::Delete));

  for (const UserRole role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(userRoleToString(role));
    CHECK_FALSE(allows(role, "/auth/users/sessions", drogon::Get));
    CHECK_FALSE(allows(role, "/auth/users/7/sessions", drogon::Get));
    CHECK_FALSE(allows(role, "/auth/users/7/sessions", drogon::Delete));
    CHECK_FALSE(allows(role, kOne, drogon::Delete));
  }
}

TEST_CASE("a session route pattern matches one segment per placeholder")
{
  using role_access::routeMatches;
  CHECK(routeMatches("/auth/users/{id}/sessions", "/auth/users/12/sessions"));
  CHECK(routeMatches("/auth/users/{id}/sessions/{id}", "/auth/users/12/sessions/ab"));
  CHECK_FALSE(routeMatches("/auth/users/{id}/sessions", "/auth/users//sessions"));
  CHECK_FALSE(routeMatches("/auth/users/{id}/sessions", "/auth/users/1/2/sessions"));
  CHECK_FALSE(routeMatches("/auth/users/{id}/sessions", "/auth/users/1/sessions/"));
  CHECK_FALSE(routeMatches("/auth/users/{id}/sessions", "/auth/users/1"));
  CHECK_FALSE(routeMatches("/auth/sessions", "/auth/sessions/x"));
  CHECK_FALSE(routeMatches("/auth/sessions", ""));
}
