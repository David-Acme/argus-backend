#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <cstddef>
#include <doctest/doctest.h>
#include <string>
#include <sync/sync-errors.hxx>
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
    {"UserAccountDisabled", &SyncErrors::UserAccountDisabled,
     ErrorCode::Unauthorized, 401, "User account is disabled"},
    {"MissingMessageType", &SyncErrors::MissingMessageType,
     ErrorCode::BadRequest, 400, "Missing message type"},
    {"UnknownMessageType", &SyncErrors::UnknownMessageType,
     ErrorCode::BadRequest, 400, "Unknown message type"},
    {"NotificationSyncUnavailable", &SyncErrors::NotificationSyncUnavailable,
     ErrorCode::ServiceUnavailable, 503, "Notification sync unavailable"},
    {"CameraSyncUnavailable", &SyncErrors::CameraSyncUnavailable,
     ErrorCode::ServiceUnavailable, 503, "Camera sync unavailable"},
    {"ProductivitySyncUnavailable", &SyncErrors::ProductivitySyncUnavailable,
     ErrorCode::ServiceUnavailable, 503, "Productivity sync unavailable"},
    {.name = "IdentitySyncUnavailable",
     .definition = &SyncErrors::IdentitySyncUnavailable,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "Identity sync unavailable"},
    {"VoiceUnavailable", &SyncErrors::VoiceUnavailable,
     ErrorCode::ServiceUnavailable, 503, "Voice unavailable"},
    {.name = "TooManyFrames",
     .definition = &SyncErrors::TooManyFrames,
     .code = ErrorCode::TooManyRequests,
     .status = 429,
     .message = "Too many messages on this socket"},
    {.name = "ReplicaTooOld",
     .definition = &SyncErrors::ReplicaTooOld,
     .code = ErrorCode::Conflict,
     .status = 409,
     .message = "Audit cursor is older than the retention window"},
    {.name = "SyncStopping",
     .definition = &SyncErrors::SyncStopping,
     .code = ErrorCode::ServiceUnavailable,
     .status = 503,
     .message = "Sync is shutting down"},
};

constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxCodeBytes = 128;

TEST_CASE("the sync catalog matches the table pinned here")
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

TEST_CASE("every sync entry is legal on the wire")
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

TEST_CASE("no two sync entries say the same thing")
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
