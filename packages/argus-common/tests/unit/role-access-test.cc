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
        CHECK(role_access::hasAccess(UserRole::Owner, name,
                                     RolePermission::Read));
        CHECK(role_access::hasAccess(UserRole::Owner, name,
                                     RolePermission::Create));
        CHECK(role_access::hasAccess(UserRole::Owner, name,
                                     RolePermission::Update));
        CHECK(role_access::hasAccess(UserRole::Owner, name,
                                     RolePermission::Delete));
    }
}

TEST_CASE("Resident permissions follow the kTableAccess map")
{
    CHECK(role_access::hasAccess(UserRole::Resident, TableName::Camera,
                                 RolePermission::Read));
    CHECK(role_access::hasAccess(UserRole::Resident, TableName::Camera,
                                 RolePermission::Create));
    CHECK(role_access::hasAccess(UserRole::Resident, TableName::Camera,
                                 RolePermission::Update));
    CHECK(role_access::hasAccess(UserRole::Resident, TableName::Camera,
                                 RolePermission::Delete));

    CHECK(role_access::hasAccess(UserRole::Resident, TableName::Zone,
                                 RolePermission::Delete));
    CHECK(role_access::hasAccess(UserRole::Resident, TableName::Memory,
                                 RolePermission::Create));

    CHECK(role_access::hasAccess(UserRole::Resident, TableName::User,
                                 RolePermission::Read));
    CHECK_FALSE(role_access::hasAccess(UserRole::Resident, TableName::User,
                                       RolePermission::Create));
    CHECK_FALSE(role_access::hasAccess(UserRole::Resident, TableName::User,
                                       RolePermission::Update));
    CHECK_FALSE(role_access::hasAccess(UserRole::Resident, TableName::User,
                                       RolePermission::Delete));

    CHECK(role_access::hasAccess(UserRole::Resident, TableName::Notification,
                                 RolePermission::Read));
    CHECK(role_access::hasAccess(UserRole::Resident, TableName::Notification,
                                 RolePermission::Update));
    CHECK_FALSE(role_access::hasAccess(UserRole::Resident,
                                       TableName::Notification,
                                       RolePermission::Delete));

    CHECK_FALSE(role_access::hasAccess(UserRole::Resident,
                                       TableName::UserInvitation,
                                       RolePermission::Read));
    CHECK_FALSE(role_access::hasAccess(UserRole::Resident,
                                       TableName::NotificationToken,
                                       RolePermission::Read));
}

TEST_CASE("Guard permissions follow the kTableAccess map")
{
    CHECK(role_access::hasAccess(UserRole::Guard, TableName::Camera,
                                 RolePermission::Read));
    CHECK_FALSE(role_access::hasAccess(UserRole::Guard, TableName::Camera,
                                       RolePermission::Create));
    CHECK_FALSE(role_access::hasAccess(UserRole::Guard, TableName::Camera,
                                       RolePermission::Delete));

    CHECK(role_access::hasAccess(UserRole::Guard, TableName::Person,
                                 RolePermission::Read));
    CHECK_FALSE(role_access::hasAccess(UserRole::Guard, TableName::Person,
                                       RolePermission::Update));

    CHECK_FALSE(role_access::hasAccess(UserRole::Guard, TableName::Reminder,
                                       RolePermission::Read));
    CHECK_FALSE(role_access::hasAccess(UserRole::Guard, TableName::Memory,
                                       RolePermission::Read));
}

TEST_CASE("Guest permissions follow the kTableAccess map")
{
    CHECK(role_access::hasAccess(UserRole::Guest, TableName::Camera,
                                 RolePermission::Read));
    CHECK_FALSE(role_access::hasAccess(UserRole::Guest, TableName::Camera,
                                       RolePermission::Update));

    CHECK_FALSE(role_access::hasAccess(UserRole::Guest, TableName::Person,
                                       RolePermission::Read));

    CHECK(role_access::hasAccess(UserRole::Guest,
                                 TableName::NotificationToken,
                                 RolePermission::Create));
    CHECK_FALSE(role_access::hasAccess(UserRole::Guest,
                                       TableName::NotificationToken,
                                       RolePermission::Read));
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
    CHECK(role_access::hasHttpAccess(UserRole::Owner, "/camera",
                                     drogon::Delete));
    CHECK(role_access::hasHttpAccess(UserRole::Owner, "/auth/logout",
                                     drogon::Delete));
    CHECK(role_access::hasHttpAccess(UserRole::Owner, "/anything", drogon::Get));
}

TEST_CASE("hasHttpAccess enforces table permissions per role")
{
    CHECK(role_access::hasHttpAccess(UserRole::Resident, "/camera/1",
                                     drogon::Get));
    CHECK(role_access::hasHttpAccess(UserRole::Resident, "/camera/1",
                                     drogon::Delete));
    CHECK(role_access::hasHttpAccess(UserRole::Resident, "/zone",
                                     drogon::Post));
    CHECK_FALSE(role_access::hasHttpAccess(UserRole::Guest, "/camera/1",
                                           drogon::Delete));
    CHECK(role_access::hasHttpAccess(UserRole::Guard, "/camera/1",
                                     drogon::Get));
    CHECK_FALSE(role_access::hasHttpAccess(UserRole::Guard, "/camera/1",
                                           drogon::Post));
    CHECK(role_access::hasHttpAccess(UserRole::Guest, "/camera", drogon::Get));
    CHECK_FALSE(role_access::hasHttpAccess(UserRole::Guest, "/reminder",
                                           drogon::Get));
    CHECK_FALSE(role_access::hasHttpAccess(UserRole::Guest, "/sync",
                                           drogon::Get));
}

TEST_CASE("hasHttpAccess applies kAuthAccess to /auth paths")
{
    CHECK(role_access::hasHttpAccess(UserRole::Resident, "/auth/login",
                                     drogon::Post));
    CHECK(role_access::hasHttpAccess(UserRole::Resident, "/auth/me",
                                     drogon::Get));
    CHECK(role_access::hasHttpAccess(UserRole::Resident, "/auth/profile",
                                     drogon::Patch));
    CHECK_FALSE(role_access::hasHttpAccess(UserRole::Resident, "/auth/logout",
                                           drogon::Delete));

    CHECK(role_access::hasHttpAccess(UserRole::Guard, "/auth/me", drogon::Get));
    CHECK_FALSE(role_access::hasHttpAccess(UserRole::Guard, "/auth/login",
                                           drogon::Post));

    CHECK(role_access::hasHttpAccess(UserRole::Guest, "/auth/me", drogon::Get));
    CHECK_FALSE(role_access::hasHttpAccess(UserRole::Guest, "/auth/logout",
                                           drogon::Delete));
}
