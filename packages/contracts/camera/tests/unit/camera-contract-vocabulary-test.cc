#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <camera/camera-driver.hxx>
#include <camera/camera-record-mode.hxx>
#include <camera/event-severity.hxx>
#include <camera/identity-state.hxx>
#include <camera/zone-type.hxx>

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

TEST_CASE("event severity strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {EventSeverity::Info, EventSeverity::Warning,
                   EventSeverity::Critical},
        .names = {"info", "warning", "critical"},
        .toString = eventSeverityToString,
        .fromString = eventSeverityFromString});
}

TEST_CASE("camera record mode strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {CameraRecordMode::Events, CameraRecordMode::Continuous},
        .names = {"events", "continuous"},
        .toString = cameraRecordModeToString,
        .fromString = cameraRecordModeFromString});
}

TEST_CASE("zone type strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {ZoneType::Monitor, ZoneType::Alert, ZoneType::Exclude},
        .names = {"monitor", "alert", "exclude"},
        .toString = zoneTypeToString,
        .fromString = zoneTypeFromString});
}

TEST_CASE("identity state strings round-trip and fail closed")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {IdentityState::Known, IdentityState::Unrecognized,
                   IdentityState::Unobservable},
        .names = {"known", "unrecognized", "unobservable"},
        .toString = identityStateToString,
        .fromString = identityStateFromString});

    // Anything a producer invents reads as unrecognized, never as known.
    CHECK(identityStateFromString("") == IdentityState::Unrecognized);
    CHECK(identityStateFromString("KNOWN") == IdentityState::Unrecognized);
    CHECK(identityStateFromString("identified") == IdentityState::Unrecognized);
}

TEST_CASE("camera driver strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {CameraDriver::Tapo, CameraDriver::Onvif, CameraDriver::Rtsp},
        .names = {"tapo", "onvif", "rtsp"},
        .toString = cameraDriverToString,
        .fromString = cameraDriverFromString});
}

TEST_CASE("unknown strings fall back to documented defaults")
{
    CHECK(eventSeverityFromString("bogus") == EventSeverity::Info);
    CHECK(cameraRecordModeFromString("bogus") == CameraRecordMode::Events);
    CHECK(zoneTypeFromString("bogus") == ZoneType::Monitor);
    CHECK(cameraDriverFromString("bogus") == CameraDriver::Tapo);
}
