#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <array>
#include <cstddef>
#include <doctest/doctest.h>
#include <productivity/productivity-errors.hxx>
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
    {.name = "ProjectNotFound",
     .definition = &ProductivityErrors::ProjectNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Project not found"},
    {.name = "TaskNotFound",
     .definition = &ProductivityErrors::TaskNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Task not found"},
    {.name = "CalendarEventNotFound",
     .definition = &ProductivityErrors::CalendarEventNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Calendar event not found"},
    {.name = "ShareNotFound",
     .definition = &ProductivityErrors::ShareNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "Share not found"},
    {.name = "UserNotFound",
     .definition = &ProductivityErrors::UserNotFound,
     .code = ErrorCode::NotFound,
     .status = 404,
     .message = "User not found"},
    {.name = "CannotSeeProjects",
     .definition = &ProductivityErrors::CannotSeeProjects,
     .code = ErrorCode::Forbidden,
     .status = 403,
     .message = "That user cannot see projects"},
    {.name = "CannotSeeCalendarEvents",
     .definition = &ProductivityErrors::CannotSeeCalendarEvents,
     .code = ErrorCode::Forbidden,
     .status = 403,
     .message = "That user cannot see calendar events"},
    {.name = "OwnerAlreadyHasAccess",
     .definition = &ProductivityErrors::OwnerAlreadyHasAccess,
     .code = ErrorCode::Conflict,
     .status = 409,
     .message = "The owner already has access"},
    {.name = "ChangeNotRecorded",
     .definition = &ProductivityErrors::ChangeNotRecorded,
     .code = ErrorCode::InternalError,
     .status = 500,
     .message = "The change could not be recorded"},
}};

constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxCodeBytes = 128;

TEST_CASE("the productivity catalog matches the table pinned here")
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

TEST_CASE("every productivity entry is legal on the wire")
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

TEST_CASE("no two productivity entries say the same thing")
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
