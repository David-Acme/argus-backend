#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <array>
#include <cstddef>
#include <doctest/doctest.h>
#include <string>
#include <vlm/vlm-errors.hxx>

struct CatalogEntry
{
  const char* name;
  const ErrorDefinition* definition;
  ErrorCode code;
  int status;
  const char* message;
};

constexpr std::array<CatalogEntry, 11> kCatalog{{
    {.name = "VisionEngineNotLoaded",
     .definition = &VlmErrors::VisionEngineNotLoaded,
     .code = ErrorCode::VlmNotLoaded,
     .status = 503,
     .message = "Vision engine is not loaded"},
    {.name = "BodyNotJsonObject",
     .definition = &VlmErrors::BodyNotJsonObject,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Body must be a JSON object"},
    {.name = "InvalidRequest",
     .definition = &VlmErrors::InvalidRequest,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Invalid vision request"},
    {.name = "ImageNotDecodable",
     .definition = &VlmErrors::ImageNotDecodable,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Image is not decodable"},
    {.name = "Unauthorized",
     .definition = &VlmErrors::Unauthorized,
     .code = ErrorCode::Unauthorized,
     .status = 401,
     .message = "Service credential required"},
    {.name = "Cancelled",
     .definition = &VlmErrors::Cancelled,
     .code = ErrorCode::Cancelled,
     .status = 499,
     .message = "Vision caption cancelled"},
    {.name = "DeadlineExceeded",
     .definition = &VlmErrors::DeadlineExceeded,
     .code = ErrorCode::DeadlineExceeded,
     .status = 504,
     .message = "Vision caption deadline exceeded"},
    {.name = "Busy",
     .definition = &VlmErrors::Busy,
     .code = ErrorCode::TooManyRequests,
     .status = 429,
     .message = "Vision caption busy"},
    {.name = "InternalError",
     .definition = &VlmErrors::InternalError,
     .code = ErrorCode::InternalError,
     .status = 500,
     .message = "Vision caption failed"},
    {.name = "InvalidResponse",
     .definition = &VlmErrors::InvalidResponse,
     .code = ErrorCode::BadGateway,
     .status = 502,
     .message = "Invalid vision response"},
    {.name = "Unavailable",
     .definition = &VlmErrors::Unavailable,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "Vision service unavailable"},
}};

constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxCodeBytes = 128;

TEST_CASE("the vlm catalog matches the table pinned here")
{
  for (const auto& entry : kCatalog) {
    CAPTURE(entry.name);
    CHECK(std::string(entry.definition->wireCode()) ==
          std::string(toString(entry.code)));
    CHECK(entry.definition->status == entry.status);
    CHECK(std::string(entry.definition->message) == entry.message);
  }
  CHECK(kCatalog.size() == 11);
}

TEST_CASE("every vlm entry is legal on the wire")
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

TEST_CASE("no two vlm entries say the same thing")
{
  for (std::size_t i = 0; i < kCatalog.size(); ++i) {
    for (std::size_t j = i + 1; j < kCatalog.size(); ++j) {
      CAPTURE(i);
      CAPTURE(j);
      CHECK(std::string(kCatalog[i].definition->message) !=
            std::string(kCatalog[j].definition->message));
    }
  }
}
