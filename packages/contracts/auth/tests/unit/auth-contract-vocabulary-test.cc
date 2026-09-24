#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/device-login-status.hxx>
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

TEST_CASE("unknown strings fall back to documented defaults")
{
    CHECK(userRoleFromString("bogus") == UserRole::Guest);
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
