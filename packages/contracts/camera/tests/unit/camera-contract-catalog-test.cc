#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <array>
#include <camera/camera-errors.hxx>
#include <cstddef>
#include <doctest/doctest.h>
#include <string>

struct CatalogEntry
{
  const char* name;
  const ErrorDefinition* definition;
  ErrorCode code;
  int status;
  const char* message;
};

constexpr std::array<CatalogEntry, 9> kCatalog{{
    {.name = "Forbidden",
     .definition = &CameraErrors::Forbidden,
     .code = ErrorCode::Forbidden,
     .status = 403,
     .message = "Forbidden"},
    {.name = "InvalidCameraId",
     .definition = &CameraErrors::InvalidCameraId,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Invalid cameraId"},
    {.name = "CameraNotFound",
     .definition = &CameraErrors::CameraNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Camera not found"},
    {.name = "TooManyCameraSubscriptions",
     .definition = &CameraErrors::TooManyCameraSubscriptions,
     .code = ErrorCode::TooManyRequests,
     .status = 429,
     .message = "Too many camera subscriptions"},
    {.name = "TooManyViewers",
     .definition = &CameraErrors::TooManyViewers,
     .code = ErrorCode::TooManyRequests,
     .status = 429,
     .message = "too_many_viewers"},
    {.name = "SubscribeFailed",
     .definition = &CameraErrors::SubscribeFailed,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "subscribe_failed"},
    {.name = "ZoneNotFound",
     .definition = &CameraErrors::ZoneNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Zone not found"},
    {.name = "CameraUnreachable",
     .definition = &CameraErrors::CameraUnreachable,
     .code = ErrorCode::CameraUnreachable,
     .status = 502,
     .message = "The camera refused the command"},
    {.name = "ChangeNotRecorded",
     .definition = &CameraErrors::ChangeNotRecorded,
     .code = ErrorCode::InternalError,
     .status = 500,
     .message = "The change could not be recorded"},
}};

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
  CHECK(kCatalog.size() == 9);
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
