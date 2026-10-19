#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <drogon/HttpTypes.h>
#include <auth/role-access.hxx>
#include <sync/table-name.hxx>
#include <auth/user-role.hxx>
#include <string>
#include <string_view>
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
    CHECK(role_access::hasAccess({.role = UserRole::Resident,
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

    CHECK(role_access::hasAccess({.role = UserRole::Guard,
                                  .table = TableName::Reminder,
                                  .perm = RolePermission::Read}));
    CHECK(role_access::hasAccess({.role = UserRole::Guard,
                                  .table = TableName::ReminderDetail,
                                  .perm = RolePermission::Update}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Guard,
                                        .table = TableName::Memory,
                                        .perm = RolePermission::Read}));
    CHECK(role_access::hasAccess({.role = UserRole::Guard,
                                  .table = TableName::Notification,
                                  .perm = RolePermission::Delete}));
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
                                  .table = TableName::Reminder,
                                  .perm = RolePermission::Delete}));
    CHECK(role_access::hasAccess({.role = UserRole::Guest,
                                  .table = TableName::ReminderDetail,
                                  .perm = RolePermission::Read}));

    CHECK(role_access::hasAccess({.role = UserRole::Guest,
                                  .table = TableName::NotificationToken,
                                  .perm = RolePermission::Create}));
    CHECK_FALSE(role_access::hasAccess({.role = UserRole::Guest,
                                        .table = TableName::NotificationToken,
                                        .perm = RolePermission::Read}));
    CHECK(role_access::hasAccess({.role = UserRole::Guest,
                                  .table = TableName::Notification,
                                  .perm = RolePermission::Delete}));
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
    CHECK(resident.size() == 19);

    const auto guard = role_access::readableTables(UserRole::Guard);
    CHECK(guard.size() == 12);

    const auto guest = role_access::readableTables(UserRole::Guest);
    CHECK(guest.size() == 8);
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
    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Guest, .path = "/reminder", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess(
        {.role = UserRole::Guest, .path = "/sync", .method = drogon::Get}));
}

TEST_CASE("an /auth path that no route table lists is refused to every non-owner")
{
    for (const UserRole role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
        CAPTURE(userRoleToString(role));
        CHECK_FALSE(role_access::hasHttpAccess(
            {.role = role, .path = "/auth/login", .method = drogon::Post}));
        CHECK_FALSE(role_access::hasHttpAccess(
            {.role = role, .path = "/auth/me", .method = drogon::Get}));
        CHECK_FALSE(role_access::hasHttpAccess(
            {.role = role, .path = "/auth/profile", .method = drogon::Patch}));
        CHECK_FALSE(role_access::hasHttpAccess(
            {.role = role, .path = "/auth/logout", .method = drogon::Delete}));
        CHECK_FALSE(role_access::hasHttpAccess(
            {.role = role, .path = "/auth", .method = drogon::Get}));
    }
}

TEST_CASE("the role check reads the path the way the router does: any case, a trailing slash")
{
    const auto allows = [](UserRole role, std::string_view path, drogon::HttpMethod method) {
        return role_access::hasHttpAccess({.role = role, .path = path, .method = method});
    };

    for (const UserRole role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
        CAPTURE(userRoleToString(role));
        CHECK_FALSE(allows(role, "/auth/Users/sessions", drogon::Get));
        CHECK_FALSE(allows(role, "/AUTH/USERS/SESSIONS", drogon::Get));
        CHECK_FALSE(allows(role, "/auth/users/7/Sessions", drogon::Get));
        CHECK_FALSE(allows(role, "/auth/users/sessions/", drogon::Get));
        CHECK_FALSE(allows(role, "/auth/users/7/sessions/", drogon::Get));
        CHECK_FALSE(allows(role, "/auth/users/7/sessions/", drogon::Delete));
        CHECK_FALSE(allows(role, "/Guard/Decisions", drogon::Get));
        CHECK_FALSE(allows(role, "/Visitor-Settings", drogon::Get));
        CHECK(allows(role, "/Auth/Sessions", drogon::Get));
        CHECK(allows(role, "/auth/sessions/", drogon::Delete));
        CHECK_FALSE(allows(role, "/auth/sessions/", drogon::Get));
        CHECK(allows(role, "/Camera/3", drogon::Get));
    }

    CHECK(allows(UserRole::Resident, "/Guard/Mode", drogon::Post));
    CHECK_FALSE(allows(UserRole::Guest, "/Guard/Mode", drogon::Post));
    CHECK_FALSE(allows(UserRole::Guest, "/Camera/3", drogon::Delete));
    CHECK(allows(UserRole::Resident, "/guard/episodes", drogon::Get));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/episodes/", drogon::Get));
    CHECK_FALSE(allows(UserRole::Guard, "/Guard/Episodes/", drogon::Get));
    CHECK(allows(UserRole::Owner, "/Auth/Users/Sessions/", drogon::Get));
}

TEST_CASE("a method no table names is refused, PUT included, unless a route lists it")
{
    CHECK_FALSE(role_access::permissionForMethod(drogon::Put).has_value());
    CHECK_FALSE(role_access::permissionForMethod(drogon::Head).has_value());
    CHECK_FALSE(role_access::permissionForMethod(drogon::Options).has_value());
    for (const UserRole role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
        CAPTURE(userRoleToString(role));
        CHECK_FALSE(role_access::hasHttpAccess(
            {.role = role, .path = "/camera/1", .method = drogon::Put}));
        CHECK_FALSE(role_access::hasHttpAccess(
            {.role = role, .path = "/camera/1", .method = drogon::Head}));
        CHECK(role_access::hasHttpAccess(
            {.role = role, .path = "/privacy/me", .method = drogon::Put}));
    }
    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Resident, .path = "/guard/safety/pin", .method = drogon::Put}));
}

TEST_CASE("a table answers only its own whole first segment")
{
    CHECK_FALSE(role_access::tableFromPath("/users-x").has_value());
    CHECK_FALSE(role_access::tableFromPath("/camerax/1").has_value());
    CHECK_FALSE(role_access::tableFromPath("/notificationx").has_value());
    CHECK_FALSE(role_access::tableFromPath("/voiceprint/user/3").has_value());
    CHECK(role_access::tableFromPath("/user") == TableName::User);
    CHECK_FALSE(role_access::hasHttpAccess(
        {.role = UserRole::Resident, .path = "/users-x", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess(
        {.role = UserRole::Guest, .path = "/camerax", .method = drogon::Get}));
}

TEST_CASE("the delivery summary is the owner's alone")
{
    for (const UserRole role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
        CAPTURE(userRoleToString(role));
        CHECK_FALSE(role_access::hasHttpAccess(
            {.role = role, .path = "/notification/delivery-summary", .method = drogon::Get}));
        CHECK_FALSE(role_access::hasHttpAccess(
            {.role = role, .path = "/Notification/Delivery-Summary/", .method = drogon::Get}));
        CHECK(role_access::hasHttpAccess(
            {.role = role, .path = "/notification/responses", .method = drogon::Get}));
    }
    CHECK(role_access::hasHttpAccess(
        {.role = UserRole::Owner, .path = "/notification/delivery-summary", .method = drogon::Get}));
}

TEST_CASE("every role deletes a notification by id or a whole thread, an inactive role included")
{
    for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
        CAPTURE(userRoleToString(role));
        for (const bool roleActive : {true, false}) {
            CAPTURE(roleActive);
            for (const std::string_view path : {"/notification/5", "/notification/thread/guard:episode:41"}) {
                CAPTURE(path);
                CHECK(role_access::hasHttpAccess({.role = role,
                                                  .path = path,
                                                  .method = drogon::Delete,
                                                  .roleActive = roleActive}));
            }
        }
    }
    for (const std::string_view path : {"/notification/5", "/notification/thread/guard:episode:41"}) {
        CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Unknown,
                                                .path = path,
                                                .method = drogon::Delete,
                                                .roleActive = true}));
    }
    for (const UserRole role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
        CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/notification/responses", .method = drogon::Delete}));
    }
}

TEST_CASE("an app action answers the same as the route it stands for")
{
    using role_access::AppAction;
    for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
        CAPTURE(userRoleToString(role));
        CHECK(role_access::hasAppAction(role, AppAction::SetGuardMode) ==
              role_access::hasHttpAccess({.role = role, .path = "/guard/mode", .method = drogon::Post}));
        CHECK(role_access::hasAppAction(role, AppAction::ShowCamera) ==
              role_access::hasHttpAccess({.role = role, .path = "/camera/1", .method = drogon::Get}));
        CHECK(role_access::hasAppAction(role, AppAction::OpenScreen));
    }
    CHECK(role_access::hasAppAction(UserRole::Resident, AppAction::SetGuardMode));
    CHECK_FALSE(role_access::hasAppAction(UserRole::Guard, AppAction::SetGuardMode));
    CHECK_FALSE(role_access::hasAppAction(UserRole::Guest, AppAction::SetGuardMode));
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
  for (const auto role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(userRoleToString(role));
    CHECK_FALSE(contains(role_access::moduleTables(role), TableName::Reminder));
    CHECK_FALSE(contains(role_access::moduleTables(role), TableName::ReminderDetail));
    CHECK_FALSE(contains(role_access::moduleTables(role), TableName::ConversationMessage));
  }
}

TEST_CASE("conversation messages are own rows every registered role reads, marks read and clears, even inactive")
{
  for (const auto role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(userRoleToString(role));
    for (const bool active : {true, false}) {
      CHECK(role_access::hasAccess({.role = role, .table = TableName::ConversationMessage,
                                    .perm = RolePermission::Read, .roleActive = active}));
      CHECK(role_access::hasAccess({.role = role, .table = TableName::ConversationMessage,
                                    .perm = RolePermission::Update, .roleActive = active}));
      CHECK(role_access::hasAccess({.role = role, .table = TableName::ConversationMessage,
                                    .perm = RolePermission::Delete, .roleActive = active}));
      CHECK_FALSE(role_access::hasAccess({.role = role, .table = TableName::ConversationMessage,
                                          .perm = RolePermission::Create, .roleActive = active}));
    }
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/conversation", .method = drogon::Get, .roleActive = true}));
    CHECK(role_access::hasHttpAccess(
        {.role = role, .path = "/conversation/4/read", .method = drogon::Patch, .roleActive = true}));
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/conversation/4", .method = drogon::Delete, .roleActive = false}));
    CHECK_FALSE(role_access::hasHttpAccess(
        {.role = role, .path = "/conversation/message", .method = drogon::Post, .roleActive = true}));
  }
  CHECK(role_access::tableFromPath("/conversation/2/read") == TableName::ConversationMessage);
  CHECK_FALSE(role_access::hasAccess({.role = UserRole::Unknown, .table = TableName::ConversationMessage,
                                      .perm = RolePermission::Read, .roleActive = true}));
}

TEST_CASE("hasHttpAccess applies kGuardAccess route by route")
{
    const auto allows = [](UserRole role, std::string_view path, drogon::HttpMethod method) {
        return role_access::hasHttpAccess({.role = role, .path = path, .method = method});
    };

    CHECK(allows(UserRole::Resident, "/guard/environments", drogon::Get));
    CHECK(allows(UserRole::Resident, "/guard/mode", drogon::Post));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/mode", drogon::Get));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/environments", drogon::Post));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/environments/2", drogon::Patch));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/environments/2", drogon::Delete));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/cameras/4", drogon::Put));
    CHECK(allows(UserRole::Resident, "/guard/incidents", drogon::Get));
    CHECK(allows(UserRole::Resident, "/guard/expected-guests", drogon::Post));
    CHECK(allows(UserRole::Resident, "/guard/expected-guests", drogon::Delete));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/decisions", drogon::Get));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/decisions/summary", drogon::Get));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/decisions/7/feedback", drogon::Post));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/person/3/promote", drogon::Post));

    CHECK(allows(UserRole::Guard, "/guard/environments", drogon::Get));
    CHECK(allows(UserRole::Guard, "/guard/incidents", drogon::Get));
    CHECK(allows(UserRole::Guard, "/guard/expected-guests", drogon::Get));
    CHECK(allows(UserRole::Guard, "/guard/episodes", drogon::Get));
    CHECK_FALSE(allows(UserRole::Guard, "/guard/site", drogon::Get));
    CHECK(allows(UserRole::Resident, "/guard/episodes", drogon::Get));
    CHECK_FALSE(allows(UserRole::Guard, "/guard/environments", drogon::Post));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/episodes/4", drogon::Get));
    CHECK_FALSE(allows(UserRole::Guard, "/guard/episodes/4/review", drogon::Post));
    CHECK_FALSE(allows(UserRole::Guest, "/guard/episodes", drogon::Get));
    CHECK_FALSE(allows(UserRole::Guard, "/guard/mode", drogon::Post));
    CHECK_FALSE(allows(UserRole::Guard, "/guard/expected-guests", drogon::Post));
    CHECK_FALSE(allows(UserRole::Guard, "/guard/decisions", drogon::Get));

    CHECK_FALSE(allows(UserRole::Guest, "/guard/environments", drogon::Get));
    CHECK_FALSE(allows(UserRole::Guest, "/guard/incidents", drogon::Get));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/mode/extra", drogon::Get));
    CHECK_FALSE(allows(UserRole::Resident, "/guardian", drogon::Get));

    CHECK(allows(UserRole::Owner, "/guard/siren", drogon::Post));
    CHECK(allows(UserRole::Resident, "/guard/siren", drogon::Post));
    CHECK(allows(UserRole::Guard, "/guard/siren", drogon::Post));
    CHECK_FALSE(allows(UserRole::Guest, "/guard/siren", drogon::Post));
    CHECK_FALSE(allows(UserRole::Resident, "/guard/siren", drogon::Get));

    CHECK(allows(UserRole::Owner, "/guard/decisions", drogon::Get));
    CHECK(allows(UserRole::Owner, "/guard/environments/2", drogon::Delete));
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

  CHECK(allows(UserRole::Guard, "/auth/sessions/", drogon::Delete));
  CHECK_FALSE(allows(UserRole::Guard, "/auth/sessions/a/b", drogon::Delete));
  CHECK_FALSE(allows(UserRole::Guest, "/auth/sessions", drogon::Post));
  CHECK_FALSE(allows(UserRole::Guard, "/auth/sessionsx", drogon::Delete));
  CHECK_FALSE(allows(UserRole::Guest, "/auth/logout", drogon::Delete));
  CHECK_FALSE(allows(UserRole::Guard, "/auth/me", drogon::Get));
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

TEST_CASE("talking through a camera is open to the owner, residents and guards, never to guests")
{
  const auto allows = [](UserRole role, std::string_view path, drogon::HttpMethod method) {
    return role_access::hasHttpAccess({.role = role, .path = path, .method = method});
  };
  for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard}) {
    CAPTURE(userRoleToString(role));
    CHECK(role_access::hasCameraAction(role, role_access::CameraAction::Talk));
    CHECK(allows(role, "/camera/12/talk", drogon::Post));
  }
  CHECK_FALSE(role_access::hasCameraAction(UserRole::Guest, role_access::CameraAction::Talk));
  CHECK_FALSE(allows(UserRole::Guest, "/camera/12/talk", drogon::Post));
  CHECK_FALSE(allows(UserRole::Guard, "/camera/12/snapshot", drogon::Patch));
  CHECK_FALSE(allows(UserRole::Guard, "/camera/x/talk", drogon::Post));
  CHECK_FALSE(allows(UserRole::Guard, "/camera//talk", drogon::Post));
  CHECK_FALSE(allows(UserRole::Guard, "/camera/12/talk/more", drogon::Post));
  CHECK(allows(UserRole::Resident, "/camera/12/ptz", drogon::Patch));
}

TEST_CASE("every role that sees a camera may watch it over WebRTC, and only by POST")
{
  const auto allows = [](UserRole role, std::string_view path, drogon::HttpMethod method) {
    return role_access::hasHttpAccess({.role = role, .path = path, .method = method});
  };
  for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(userRoleToString(role));
    CHECK(role_access::hasCameraAction(role, role_access::CameraAction::Watch));
    CHECK(allows(role, "/camera/6/webrtc", drogon::Post));
  }
  for (const UserRole role : {UserRole::Guard, UserRole::Guest}) {
    CAPTURE(userRoleToString(role));
    CHECK_FALSE(allows(role, "/camera/6/webrtc", drogon::Delete));
    CHECK_FALSE(allows(role, "/camera/6/webrtc/x", drogon::Post));
    CHECK_FALSE(allows(role, "/camera/x/webrtc", drogon::Post));
  }
}

TEST_CASE("every role may ask for a realtime call token, and only by POST")
{
  for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest})
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/rtc/token", .method = drogon::Post}));
  for (const UserRole role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/rtc/token", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/rtc/rooms", .method = drogon::Post}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/rtc/token/x", .method = drogon::Post}));
  }
}

TEST_CASE("every role reads its relay credentials, only by GET")
{
  for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/rtc/ice", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/rtc/ice", .method = drogon::Post}));
  }
  CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Unknown, .path = "/rtc/ice", .method = drogon::Get}));
}

TEST_CASE("every role reads its own heartbeat, only by GET, and nothing else under /sync/")
{
  for (const UserRole role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/sync/heartbeat", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/sync/heartbeat", .method = drogon::Post}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/sync/other", .method = drogon::Get}));
  }
}

TEST_CASE("residents and guards read an environment's response list, only guards go on duty, only owners edit")
{
  for (const UserRole role : {UserRole::Resident, UserRole::Guard}) {
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/guard/environments/3/response", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/guard/environments/3/response", .method = drogon::Put}));
  }
  CHECK(role_access::hasHttpAccess({.role = UserRole::Guard, .path = "/guard/environments/3/duty", .method = drogon::Post}));
  CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Resident, .path = "/guard/environments/3/duty", .method = drogon::Post}));
  CHECK(role_access::hasHttpAccess({.role = UserRole::Owner, .path = "/guard/environments/3/response", .method = drogon::Put}));
  CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Guest, .path = "/guard/environments/3/response", .method = drogon::Get}));
  CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Guard, .path = "/guard/environments/3/response/x", .method = drogon::Get}));
  CHECK(role_access::hasHttpAccess({.role = UserRole::Guard, .path = "/guard/environments", .method = drogon::Get}));
  CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Guard, .path = "/guard/environments/3", .method = drogon::Patch}));
}

TEST_CASE("every role reads the responses that reached it and gives a verdict by PATCH")
{
  for (const UserRole role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/notification/responses", .method = drogon::Get}));
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/notification/responses/4", .method = drogon::Get}));
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/notification/responses/4", .method = drogon::Patch}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/notification/responses/4", .method = drogon::Delete}));
  }
}

TEST_CASE("every route of the notification table answers each role as pinned, in every method")
{
  constexpr auto kAllow = role_access::RouteVerdict::Allowed;
  constexpr auto kDeny = role_access::RouteVerdict::Denied;
  struct PinnedRoute
  {
    std::string_view route;
    drogon::HttpMethod method;
    bool declared;
    std::array<role_access::RouteVerdict, 4> verdicts;
  };
  const std::array<PinnedRoute, 45> pinned{{
      {.route = "/notification/ack", .method = drogon::Get, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/ack", .method = drogon::Post, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/ack", .method = drogon::Put, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/ack", .method = drogon::Patch, .declared = true, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/ack", .method = drogon::Delete, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/call-preferences", .method = drogon::Get, .declared = true, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/call-preferences", .method = drogon::Post, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/call-preferences", .method = drogon::Put, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/call-preferences", .method = drogon::Patch, .declared = true, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/call-preferences", .method = drogon::Delete, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/delivery-summary", .method = drogon::Get, .declared = true, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/delivery-summary", .method = drogon::Post, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/delivery-summary", .method = drogon::Put, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/delivery-summary", .method = drogon::Patch, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/delivery-summary", .method = drogon::Delete, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/read", .method = drogon::Get, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/read", .method = drogon::Post, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/read", .method = drogon::Put, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/read", .method = drogon::Patch, .declared = true, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/read", .method = drogon::Delete, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/responses", .method = drogon::Get, .declared = true, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/responses", .method = drogon::Post, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/responses", .method = drogon::Put, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/responses", .method = drogon::Patch, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/responses", .method = drogon::Delete, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/responses/{1}", .method = drogon::Get, .declared = true, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/responses/{1}", .method = drogon::Post, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/responses/{1}", .method = drogon::Put, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/responses/{1}", .method = drogon::Patch, .declared = true, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/responses/{1}", .method = drogon::Delete, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/thread/{1}", .method = drogon::Get, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/thread/{1}", .method = drogon::Post, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/thread/{1}", .method = drogon::Put, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/thread/{1}", .method = drogon::Patch, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/thread/{1}", .method = drogon::Delete, .declared = true, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/thread/{1}/read", .method = drogon::Get, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/thread/{1}/read", .method = drogon::Post, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/thread/{1}/read", .method = drogon::Put, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/thread/{1}/read", .method = drogon::Patch, .declared = true, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/thread/{1}/read", .method = drogon::Delete, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/{1}", .method = drogon::Get, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/{1}", .method = drogon::Post, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/{1}", .method = drogon::Put, .declared = false, .verdicts = {kAllow, kDeny, kDeny, kDeny}},
      {.route = "/notification/{1}", .method = drogon::Patch, .declared = false, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
      {.route = "/notification/{1}", .method = drogon::Delete, .declared = true, .verdicts = {kAllow, kAllow, kAllow, kAllow}},
  }};
  const std::array<UserRole, 4> roles{UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest};
  const ModuleSnapshot modules{};
  const auto verbOf = [](drogon::HttpMethod method) -> std::string_view {
    switch (method) {
    case drogon::Get:
      return "GET";
    case drogon::Post:
      return "POST";
    case drogon::Put:
      return "PUT";
    case drogon::Patch:
      return "PATCH";
    case drogon::Delete:
      return "DELETE";
    default:
      return "OTHER";
    }
  };
  for (const PinnedRoute& row : pinned) {
    std::string sample(row.route);
    if (const auto open = sample.find("{1}"); open != std::string::npos)
      sample.replace(open, 3, "5");
    CAPTURE(row.route);
    CAPTURE(verbOf(row.method));
    CAPTURE(row.declared);
    REQUIRE(role_access::tableFromPath(sample) == TableName::Notification);
    for (std::size_t index = 0; index < roles.size(); ++index) {
      CAPTURE(userRoleToString(roles.at(index)));
      CHECK(role_access::routeVerdict({.role = roles.at(index), .path = sample, .method = row.method, .modules = modules}) ==
            row.verdicts.at(index));
    }
  }
}

TEST_CASE("every role presses panic and reads the safety state; only disarmers keep PINs; only owners switch duress")
{
  for (const UserRole role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/guard/panic", .method = drogon::Post}));
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/guard/safety", .method = drogon::Get}));
    CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/guard/safety", .method = drogon::Patch}));
  }
  CHECK(role_access::hasHttpAccess({.role = UserRole::Resident, .path = "/guard/safety/pin", .method = drogon::Put}));
  CHECK(role_access::hasHttpAccess({.role = UserRole::Resident, .path = "/guard/safety/pin", .method = drogon::Delete}));
  CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Guard, .path = "/guard/safety/pin", .method = drogon::Put}));
  CHECK_FALSE(role_access::hasHttpAccess({.role = UserRole::Guest, .path = "/guard/safety/pin", .method = drogon::Put}));
  CHECK(role_access::hasHttpAccess({.role = UserRole::Owner, .path = "/guard/safety", .method = drogon::Patch}));
}

TEST_CASE("a resident and a guard may aim a camera, a guest may not")
{
  using role_access::CameraAction;
  using role_access::hasCameraAction;
  using role_access::hasHttpAccess;

  for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard}) {
    CAPTURE(userRoleToString(role));
    CHECK(hasCameraAction(role, CameraAction::Move));
    CHECK(hasCameraAction(role, CameraAction::Preset));
    CHECK(hasHttpAccess({.role = role, .path = "/camera/3/ptz", .method = drogon::Patch}));
    CHECK(hasHttpAccess({.role = role, .path = "/camera/3/preset", .method = drogon::Patch}));
  }
  for (const UserRole role : {UserRole::Guest, UserRole::Unknown}) {
    CAPTURE(userRoleToString(role));
    CHECK_FALSE(hasCameraAction(role, CameraAction::Move));
    CHECK_FALSE(hasCameraAction(role, CameraAction::Preset));
    CHECK_FALSE(hasHttpAccess({.role = role, .path = "/camera/3/ptz", .method = drogon::Patch}));
    CHECK_FALSE(hasHttpAccess({.role = role, .path = "/camera/3/preset", .method = drogon::Patch}));
  }
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Guard, .path = "/camera/3", .method = drogon::Patch}));
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Guard, .path = "/camera/3/ptz", .method = drogon::Post}));
}

TEST_CASE("only the roles the camera action table names may sound a camera siren")
{
  using role_access::CameraAction;
  using role_access::hasCameraAction;
  using role_access::hasHttpAccess;
  using role_access::RouteVerdict;
  using role_access::routeVerdict;

  for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard}) {
    CAPTURE(userRoleToString(role));
    CHECK(hasCameraAction(role, CameraAction::Siren));
    CHECK(hasHttpAccess({.role = role, .path = "/guard/siren", .method = drogon::Post}));
  }
  for (const UserRole role : {UserRole::Guest, UserRole::Unknown}) {
    CAPTURE(userRoleToString(role));
    CHECK_FALSE(hasCameraAction(role, CameraAction::Siren));
    CHECK_FALSE(hasHttpAccess({.role = role, .path = "/guard/siren", .method = drogon::Post}));
  }
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Resident, .path = "/guard/siren", .method = drogon::Get}));
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Resident, .path = "/guard/siren/1", .method = drogon::Post}));

  const ModuleFlags modules{
      {.id = std::string(role_access::kSurveillanceModule),
       .enabled = true,
       .lifecycle = "active",
       .dataPurgedAt = 0,
       .roles = {"guard"},
       .name = {},
       .summary = {},
       .intro = {},
       .kind = "optional"},
      {.id = std::string(role_access::kProductivityModule),
       .enabled = false,
       .lifecycle = "disabled",
       .dataPurgedAt = 0,
       .roles = {"resident"},
       .name = {},
       .summary = {},
       .intro = {},
       .kind = "optional"}};
  const ModuleSnapshot split(modules);
  const auto siren = [](UserRole role, const ModuleSnapshot& snapshot) {
    return routeVerdict({.role = role,
                         .path = "/guard/siren",
                         .method = drogon::Post,
                         .modules = snapshot});
  };
  CHECK(siren(UserRole::Guard, split) == RouteVerdict::Allowed);
  CHECK(routeVerdict({.role = UserRole::Owner,
                      .path = "/guard/siren",
                      .method = drogon::Post,
                      .modules = split}) == RouteVerdict::Allowed);
  CHECK(siren(UserRole::Resident, split) == RouteVerdict::RoleInactive);
  CHECK(siren(UserRole::Guest, split) == RouteVerdict::Denied);

  const ModuleSnapshot surveillanceOff(ModuleFlags{
      {.id = std::string(role_access::kSurveillanceModule),
       .enabled = false,
       .lifecycle = "disabled",
       .dataPurgedAt = 0,
       .roles = {"guard"},
       .name = {},
       .summary = {},
       .intro = {},
       .kind = "optional"}});
  for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard}) {
    CAPTURE(userRoleToString(role));
    CHECK(siren(role, surveillanceOff) == RouteVerdict::ModuleDisabled);
  }
}

TEST_CASE("guards read named visitors and their pictures; only owners manage the gallery")
{
  using role_access::hasHttpAccess;
  CHECK(hasHttpAccess({.role = UserRole::Guard, .path = "/visitor", .method = drogon::Get}));
  CHECK(hasHttpAccess({.role = UserRole::Guard, .path = "/visitor/12", .method = drogon::Get}));
  CHECK(hasHttpAccess(
      {.role = UserRole::Guard, .path = "/visitor/12/crop-preview", .method = drogon::Get}));
  CHECK(hasHttpAccess(
      {.role = UserRole::Guard, .path = "/visitor-crop/abc/content", .method = drogon::Get}));
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Guard, .path = "/visitor/12", .method = drogon::Patch}));
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Guard, .path = "/visitor/12", .method = drogon::Delete}));
  CHECK_FALSE(
      hasHttpAccess({.role = UserRole::Guard, .path = "/visitor/12/merge", .method = drogon::Post}));
  CHECK_FALSE(hasHttpAccess({.role = UserRole::Guard, .path = "/visitor-settings", .method = drogon::Get}));
  for (const auto role : {UserRole::Resident, UserRole::Guest}) {
    CHECK_FALSE(hasHttpAccess({.role = role, .path = "/visitor", .method = drogon::Get}));
    CHECK_FALSE(hasHttpAccess({.role = role, .path = "/visitor/12", .method = drogon::Get}));
  }
  CHECK(hasHttpAccess({.role = UserRole::Owner, .path = "/visitor-settings", .method = drogon::Patch}));
}

TEST_CASE("the recording state is Owner and Guard, and the module gate holds it too")
{
  const auto allows = [](UserRole role, const char* path, drogon::HttpMethod method,
                         bool roleActive = true) {
    return role_access::hasHttpAccess(
        {.role = role, .path = path, .method = method, .roleActive = roleActive});
  };

  CHECK(allows(UserRole::Owner, "/camera/recording", drogon::Get));
  CHECK(allows(UserRole::Guard, "/camera/recording", drogon::Get));
  CHECK_FALSE(allows(UserRole::Resident, "/camera/recording", drogon::Get));
  CHECK_FALSE(allows(UserRole::Guest, "/camera/recording", drogon::Get));
  CHECK_FALSE(allows(UserRole::Unknown, "/camera/recording", drogon::Get));
  CHECK_FALSE(allows(UserRole::Guard, "/camera/recording", drogon::Get, false));
  CHECK_FALSE(allows(UserRole::Guard, "/camera/recording", drogon::Post));
  CHECK(allows(UserRole::Resident, "/camera/4", drogon::Get));

  const ModuleSnapshot modules = ModuleSnapshot{ModuleFlags{
      {.id = "surveillance", .enabled = false, .roles = {"guard"}},
  }};
  CHECK(role_access::routeVerdict(
            {.role = UserRole::Owner, .path = "/camera/recording", .method = drogon::Get,
             .modules = modules}) == role_access::RouteVerdict::ModuleDisabled);
}

TEST_CASE("clip access follows the table exactly as the access rows")
{
  const auto allows = [](UserRole role, const char* path, drogon::HttpMethod method,
                         bool roleActive = true) {
    return role_access::hasHttpAccess(
        {.role = role, .path = path, .method = method, .roleActive = roleActive});
  };

  CHECK(allows(UserRole::Owner, "/camera/4/clips", drogon::Get));
  CHECK(allows(UserRole::Owner, "/camera/4/clips/7/preview", drogon::Get));
  CHECK(allows(UserRole::Owner, "/clip/abc/content", drogon::Get));
  CHECK(allows(UserRole::Owner, "/camera/4/clips/7", drogon::Delete));

  CHECK(allows(UserRole::Guard, "/camera/4/clips", drogon::Get));
  CHECK(allows(UserRole::Guard, "/camera/4/clips/7/preview", drogon::Get));
  CHECK(allows(UserRole::Guard, "/clip/abc/content", drogon::Get));
  CHECK_FALSE(allows(UserRole::Guard, "/camera/4/clips/7", drogon::Delete));
  CHECK_FALSE(allows(UserRole::Guard, "/camera/4/clips", drogon::Post));

  for (const auto role : {UserRole::Resident, UserRole::Guest}) {
    CHECK_FALSE(allows(role, "/camera/4/clips", drogon::Get));
    CHECK_FALSE(allows(role, "/camera/4/clips/7/preview", drogon::Get));
    CHECK_FALSE(allows(role, "/clip/abc/content", drogon::Get));
    CHECK_FALSE(allows(role, "/camera/4/clips/7", drogon::Delete));
    CHECK(allows(role, "/camera/4", drogon::Get));
  }

  CHECK_FALSE(allows(UserRole::Unknown, "/camera/4/clips", drogon::Get));
  CHECK_FALSE(allows(UserRole::Unknown, "/clip/abc/content", drogon::Get));

  CHECK_FALSE(allows(UserRole::Guard, "/camera/4/clips", drogon::Get, false));

  const ModuleSnapshot modules = ModuleSnapshot{ModuleFlags{
      {.id = "surveillance", .enabled = false, .roles = {"guard"}},
  }};
  CHECK(role_access::routeVerdict(
            {.role = UserRole::Guard, .path = "/camera/4/clips", .method = drogon::Get,
             .modules = modules}) == role_access::RouteVerdict::ModuleDisabled);
  CHECK(role_access::routeVerdict(
            {.role = UserRole::Owner, .path = "/clip/abc/content", .method = drogon::Get,
             .modules = modules}) == role_access::RouteVerdict::ModuleDisabled);
  CHECK(role_access::moduleOfPath("/clip/abc/content") == role_access::kSurveillanceModule);
}

TEST_CASE("editing one occurrence answers each role exactly as editing the whole series")
{
  for (const auto role : {UserRole::Owner, UserRole::Resident, UserRole::Guard,
                          UserRole::Guest, UserRole::Unknown}) {
    for (const auto* series : {"/reminder/7", "/calendar-event/7"}) {
      const std::string occurrence = std::string(series) + "/occurrence";
      CHECK(role_access::hasHttpAccess({.role = role,
                                        .path = occurrence,
                                        .method = drogon::Patch,
                                        .roleActive = true}) ==
            role_access::hasHttpAccess({.role = role,
                                        .path = series,
                                        .method = drogon::Patch,
                                        .roleActive = true}));
    }
  }
  CHECK(role_access::hasHttpAccess(
      {.role = UserRole::Guest, .path = "/reminder/7/occurrence", .method = drogon::Patch, .roleActive = true}));
  CHECK_FALSE(role_access::hasHttpAccess(
      {.role = UserRole::Guest, .path = "/calendar-event/7/occurrence", .method = drogon::Patch, .roleActive = true}));
}
