#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <array>
#include <cstddef>
#include <doctest/doctest.h>
#include <stt/stt-errors.hxx>
#include <string>

struct CatalogEntry
{
  const char* name;
  const ErrorDefinition* definition;
  ErrorCode code;
  int status;
  const char* message;
};

constexpr std::array<CatalogEntry, 11> kCatalog{{
    {.name = "SpeechEngineNotLoaded",
     .definition = &SttErrors::SpeechEngineNotLoaded,
     .code = ErrorCode::SttNotLoaded,
     .status = 503,
     .message = "Speech-to-text engine is not loaded"},
    {.name = "InvalidRequest",
     .definition = &SttErrors::InvalidRequest,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Invalid transcription request"},
    {.name = "BodyNotPcmS16",
     .definition = &SttErrors::BodyNotPcmS16,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "Body must be audio/x-argus-pcm-s16"},
    {.name = "PcmBodyMisaligned",
     .definition = &SttErrors::PcmBodyMisaligned,
     .code = ErrorCode::BadRequest,
     .status = 400,
     .message = "PCM body is not int16-aligned"},
    {.name = "Unauthorized",
     .definition = &SttErrors::Unauthorized,
     .code = ErrorCode::Unauthorized,
     .status = 401,
     .message = "Service credential required"},
    {.name = "Cancelled",
     .definition = &SttErrors::Cancelled,
     .code = ErrorCode::Cancelled,
     .status = 499,
     .message = "Transcription cancelled"},
    {.name = "DeadlineExceeded",
     .definition = &SttErrors::DeadlineExceeded,
     .code = ErrorCode::DeadlineExceeded,
     .status = 504,
     .message = "Transcription deadline exceeded"},
    {.name = "Busy",
     .definition = &SttErrors::Busy,
     .code = ErrorCode::TooManyRequests,
     .status = 429,
     .message = "Transcription busy"},
    {.name = "InternalError",
     .definition = &SttErrors::InternalError,
     .code = ErrorCode::InternalError,
     .status = 500,
     .message = "Transcription failed"},
    {.name = "InvalidResponse",
     .definition = &SttErrors::InvalidResponse,
     .code = ErrorCode::BadGateway,
     .status = 502,
     .message = "Invalid transcription response"},
    {.name = "Unavailable",
     .definition = &SttErrors::Unavailable,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "Speech-to-text service unavailable"},
}};

constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxCodeBytes = 128;

TEST_CASE("the stt catalog matches the table pinned here")
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

TEST_CASE("every stt entry is legal on the wire")
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

TEST_CASE("no two stt entries say the same thing")
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
