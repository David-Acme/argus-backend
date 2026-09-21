#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <cstddef>
#include <doctest/doctest.h>
#include <string>
#include <tts/tts-errors.hxx>
#include <vector>

// The text-to-speech boundary, pinned as a table. Naming every entry here is
// the point: an edited status, or a message that quietly loses a word, has to
// be edited twice -- here and in the header -- so the diff shows it.
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

// What response-rpc.cc's validRecord accepts before a refusal can be
// serialized: an entry outside these limits can never reach a client, the
// serializer would answer 502 for it instead.
constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxCodeBytes = 128;

TEST_CASE("the tts catalog matches the table pinned here")
{
  for (const auto& entry : kCatalog) {
    CAPTURE(entry.name);
    // The code column is compared as its wire string, which is the value
    // the errors package pins one-to-one for every enumerator: the same
    // assertion, and a failure that prints NOT_FOUND instead of a number.
    CHECK(std::string(entry.definition->wireCode()) ==
          std::string(toString(entry.code)));
    CHECK(entry.definition->status == entry.status);
    CHECK(std::string(entry.definition->message) == entry.message);
  }
  // A new entry in the header compiles and fails nothing above, because
  // nothing above knows the catalog grew. This is the tripwire: the count
  // only holds once the entry is in the table too.
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
    // The serializer's rule for a message, mirrored exactly: non-empty, at
    // most 1024 bytes, no NUL. Deliberately not restricted to ASCII -- a
    // UTF-8 message is legal on the wire, and a suite that forbade one would
    // block a legitimate translation.
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
