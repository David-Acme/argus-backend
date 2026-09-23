#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <camera/camera-errors.hxx>
#include <cstddef>
#include <doctest/doctest.h>
#include <string>
#include <vector>

struct CatalogEntry
{
  const char* name;
  const ErrorDefinition* definition;
  ErrorCode code;
  int status;
  const char* message;
};

const std::vector<CatalogEntry> kCatalog{
    {"Forbidden", &CameraErrors::Forbidden, ErrorCode::Forbidden, 403,
     "Forbidden"},
    {"InvalidCameraId", &CameraErrors::InvalidCameraId, ErrorCode::BadRequest,
     400, "Invalid cameraId"},
    {"CameraNotFound", &CameraErrors::CameraNotFound, ErrorCode::NotFound, 404,
     "Camera not found"},
    {"TooManyCameraSubscriptions", &CameraErrors::TooManyCameraSubscriptions,
     ErrorCode::TooManyRequests, 429, "Too many camera subscriptions"},
    {"TooManyViewers", &CameraErrors::TooManyViewers,
     ErrorCode::TooManyRequests, 429, "too_many_viewers"},
    {"SubscribeFailed", &CameraErrors::SubscribeFailed,
     ErrorCode::ServiceUnavailable, 503, "subscribe_failed"},
    {"ZoneNotFound", &CameraErrors::ZoneNotFound, ErrorCode::NotFound, 404,
     "Zone not found"},
    {"CameraUnreachable", &CameraErrors::CameraUnreachable,
     ErrorCode::CameraUnreachable, 502, "The camera refused the command"},
};

constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxCodeBytes = 128;

TEST_CASE("the camera catalog matches the table pinned here")
{
  for (const auto& entry : kCatalog) {
    CAPTURE(entry.name);
    CHECK(std::string(entry.definition->wireCode()) ==
          std::string(toString(entry.code)));
    CHECK(entry.definition->status == entry.status);
    CHECK(std::string(entry.definition->message) == entry.message);
  }
  CHECK(kCatalog.size() == 8);
}

TEST_CASE("every camera entry is legal on the wire")
{
  for (const auto& entry : kCatalog) {
    CAPTURE(entry.name);
    const std::string message(entry.definition->message);
    const std::string wire(entry.definition->wireCode());
    CHECK(entry.definition->status >= 400);
    CHECK(entry.definition->status <= 599);
    CHECK(!message.empty());
    CHECK(message.size() <= kMaxMessageBytes);
    CHECK(message.find('\0') == std::string::npos);
    CHECK(!wire.empty());
    CHECK(wire.size() <= kMaxCodeBytes);
    CHECK(std::ranges::all_of(wire, [](unsigned char byte) {
      return byte >= 0x21 && byte <= 0x7e;
    }));
  }
}

TEST_CASE("no two camera entries say the same thing")
{
  for (std::size_t i = 0; i < kCatalog.size(); ++i) {
    for (std::size_t j = i + 1; j < kCatalog.size(); ++j) {
      CAPTURE(i);
      CAPTURE(j);
      CHECK(std::string(kCatalog[i].definition->message) !=
            kCatalog[j].definition->message);
    }
  }
}
