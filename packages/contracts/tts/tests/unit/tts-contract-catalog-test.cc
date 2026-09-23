#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <cstddef>
#include <doctest/doctest.h>
#include <string>
#include <tts/tts-errors.hxx>
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
    {"TtsNotLoaded", &TtsErrors::TtsNotLoaded, ErrorCode::TtsNotLoaded, 503,
     "Text-to-speech engine is not loaded"},
    {"InvalidRequest", &TtsErrors::InvalidRequest, ErrorCode::BadRequest, 400,
     "Invalid synthesis request"},
    {"Unauthorized", &TtsErrors::Unauthorized, ErrorCode::Unauthorized, 401,
     "Service credential required"},
    {"Cancelled", &TtsErrors::Cancelled, ErrorCode::Cancelled, 499,
     "Synthesis cancelled"},
    {"DeadlineExceeded", &TtsErrors::DeadlineExceeded,
     ErrorCode::DeadlineExceeded, 504, "Synthesis deadline exceeded"},
    {"Busy", &TtsErrors::Busy, ErrorCode::TooManyRequests, 429,
     "Synthesis busy"},
    {"InternalError", &TtsErrors::InternalError, ErrorCode::InternalError, 500,
     "Synthesis failed"},
    {"InvalidResponse", &TtsErrors::InvalidResponse, ErrorCode::BadGateway, 502,
     "Invalid synthesis response"},
    {"Unavailable", &TtsErrors::Unavailable, ErrorCode::ServiceUnavailable, 503,
     "Text-to-speech service unavailable"},
};

constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxCodeBytes = 128;

TEST_CASE("the tts catalog matches the table pinned here")
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

TEST_CASE("every tts entry is legal on the wire")
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

TEST_CASE("no two tts entries say the same thing")
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
