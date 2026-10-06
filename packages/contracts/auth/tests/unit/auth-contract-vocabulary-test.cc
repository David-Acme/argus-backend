#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/device-login-status.hxx>
#include <auth/session-origin.hxx>
#include <auth/session-platform.hxx>
#include <auth/user-role.hxx>

#include <cstddef>
#include <string>
#include <vector>

template <typename Enum>
struct CheckRoundTripInput
{
    const std::vector<Enum>& values;
    const std::vector<std::string>& names;
    std::string (*toString)(Enum);
    Enum (*fromString)(const std::string&);
};

template <typename Enum>
static void checkRoundTrip(const CheckRoundTripInput<Enum>& input)
{
    const auto& values = input.values;
    const auto& names = input.names;
    auto toString = input.toString;
    auto fromString = input.fromString;

    REQUIRE(values.size() == names.size());
    for (size_t i = 0; i < values.size(); ++i) {
        CHECK(toString(values[i]) == names[i]);
        CHECK(fromString(names[i]) == values[i]);
    }
}

TEST_CASE("user role strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {UserRole::Owner, UserRole::Resident, UserRole::Guard,
                   UserRole::Guest},
        .names = {"owner", "resident", "guard", "guest"},
        .toString = userRoleToString,
        .fromString = userRoleFromString});
}

TEST_CASE("a role name outside the enum is Unknown, never a role with permissions")
{
    for (const auto* name : {"bogus", "", "Owner", "GUARD", "owner ", "unknown", "agronomist"}) {
        CAPTURE(name);
        CHECK(userRoleFromString(name) == UserRole::Unknown);
        CHECK_FALSE(parseUserRole(name).has_value());
    }
    CHECK_FALSE(userRoleKnown(UserRole::Unknown));
    for (const auto role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest})
        CHECK(userRoleKnown(role));
    CHECK(userRoleToString(UserRole::Unknown) == "unknown");
    CHECK(parseUserRole("guard") == UserRole::Guard);
    CHECK(parseUserRole("owner") == UserRole::Owner);
}

TEST_CASE("device login status strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {DeviceLoginStatus::Pending, DeviceLoginStatus::Approved,
                   DeviceLoginStatus::Expired},
        .names = {"pending", "approved", "expired"},
        .toString = deviceLoginStatusToString,
        .fromString = deviceLoginStatusFromString});
}

TEST_CASE("unknown device login status strings fall back to pending")
{
    CHECK(deviceLoginStatusFromString("bogus") == DeviceLoginStatus::Pending);
}

TEST_CASE("session platform strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {SessionPlatform::Unknown, SessionPlatform::Android,
                   SessionPlatform::Ios, SessionPlatform::Desktop,
                   SessionPlatform::Web},
        .names = {"unknown", "android", "ios", "desktop", "web"},
        .toString = sessionPlatformToString,
        .fromString = sessionPlatformFromString});
}

TEST_CASE("an unknown session platform string falls back to unknown")
{
    CHECK(sessionPlatformFromString("Android") == SessionPlatform::Unknown);
    CHECK(sessionPlatformFromString("") == SessionPlatform::Unknown);
}

TEST_CASE("session origin strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {SessionOrigin::Unknown, SessionOrigin::Lan,
                   SessionOrigin::Tunnel, SessionOrigin::Loopback,
                   SessionOrigin::External},
        .names = {"unknown", "lan", "tunnel", "loopback", "external"},
        .toString = sessionOriginToString,
        .fromString = sessionOriginFromString});
}

TEST_CASE("an unknown session origin string falls back to unknown")
{
    CHECK(sessionOriginFromString("LAN") == SessionOrigin::Unknown);
    CHECK(sessionOriginFromString("") == SessionOrigin::Unknown);
}
