#include <doctest/doctest.h>

#include <camera/camera-driver.hxx>
#include <camera/camera-clip-trigger.hxx>
#include <camera/camera-privacy-state.hxx>
#include <camera/camera-record-mode.hxx>
#include <camera/camera-row-projection.hxx>
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

TEST_CASE("the privacy state round-trips and refuses what it does not know")
{
  CHECK(cameraPrivacyStateToString(CameraPrivacyState::On) == "on");
  CHECK(cameraPrivacyStateToString(CameraPrivacyState::Off) == "off");
  CHECK(cameraPrivacyStateToString(CameraPrivacyState::Unknown) == "unknown");
  for (int value = 0; value <= static_cast<int>(CameraPrivacyState::Off); ++value)
  {
    const auto state = static_cast<CameraPrivacyState>(value);
    CHECK(cameraPrivacyStateFromString(cameraPrivacyStateToString(state)) == state);
  }
  CHECK(cameraPrivacyStateFromString("masked") == std::nullopt);
  CHECK(cameraPrivacyStateFromString("") == std::nullopt);
  CHECK(cameraPrivacyMasked(CameraPrivacyState::On));
  CHECK_FALSE(cameraPrivacyMasked(CameraPrivacyState::Off));
  CHECK_FALSE(cameraPrivacyMasked(CameraPrivacyState::Unknown));
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
        .values = {CameraRecordMode::Off, CameraRecordMode::Motion,
                   CameraRecordMode::Continuous, CameraRecordMode::Events},
        .names = {"off", "motion", "continuous", "events"},
        .toString = cameraRecordModeToString,
        .fromString = cameraRecordModeFromString});
}

TEST_CASE("camera clip trigger strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {CameraClipTrigger::Motion, CameraClipTrigger::Event,
                   CameraClipTrigger::Continuous},
        .names = {"motion", "event", "continuous"},
        .toString = cameraClipTriggerToString,
        .fromString = cameraClipTriggerFromString});
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

TEST_CASE("a reduced camera row and diff carry no connection detail")
{
  Json::Value row(Json::objectValue);
  row["id"] = 3;
  row["name"] = "Patio";
  row["ip"] = "192.168.1.20";
  row["port"] = 554;
  row["username"] = "admin";
  row["cloudUsername"] = "owner@example.com";
  row["config"] = R"({"stream":"rtsp://admin:pw@192.168.1.20"})";
  camera_projection::reduceRow(row);
  CHECK(row["name"] == "Patio");
  CHECK(row["ip"] == "");
  CHECK(row["port"] == 0);
  CHECK(row["username"] == "");
  CHECK(row["cloudUsername"] == "");
  CHECK(row["config"] == "{}");

  Json::Value changes(Json::objectValue);
  changes["name"]["current"] = "Jardin";
  changes["ip"]["current"] = "10.0.0.2";
  changes["port"]["current"] = 8554;
  changes["config.stream"]["current"] = "rtsp://x";
  changes["portrait"]["current"] = true;
  camera_projection::reduceDiff(changes);
  CHECK(changes.isMember("name"));
  CHECK(changes.isMember("portrait"));
  CHECK_FALSE(changes.isMember("ip"));
  CHECK_FALSE(changes.isMember("port"));
  CHECK_FALSE(changes.isMember("config.stream"));
}
