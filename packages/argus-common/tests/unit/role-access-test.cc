#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/HttpTypes.h>
#include <shared/access/role-access.hxx>
#include <shared/enums.hxx>

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
