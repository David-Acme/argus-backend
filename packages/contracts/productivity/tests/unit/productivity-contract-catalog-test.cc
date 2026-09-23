#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <cstddef>
#include <doctest/doctest.h>
#include <productivity/productivity-errors.hxx>
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
    {"ProjectNotFound", &ProductivityErrors::ProjectNotFound,
     ErrorCode::NotFound, 404, "Project not found"},
    {"TaskNotFound", &ProductivityErrors::TaskNotFound, ErrorCode::NotFound,
     404, "Task not found"},
    {"CalendarEventNotFound", &ProductivityErrors::CalendarEventNotFound,
     ErrorCode::NotFound, 404, "Calendar event not found"},
    {"ShareNotFound", &ProductivityErrors::ShareNotFound, ErrorCode::NotFound,
     404, "Share not found"},
    {"UserNotFound", &ProductivityErrors::UserNotFound, ErrorCode::NotFound,
     404, "User not found"},
    {"CannotSeeProjects", &ProductivityErrors::CannotSeeProjects,
     ErrorCode::Forbidden, 403, "That user cannot see projects"},
    {"CannotSeeCalendarEvents", &ProductivityErrors::CannotSeeCalendarEvents,
     ErrorCode::Forbidden, 403, "That user cannot see calendar events"},
    {"OwnerAlreadyHasAccess", &ProductivityErrors::OwnerAlreadyHasAccess,
     ErrorCode::Conflict, 409, "The owner already has access"},
};

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
  CHECK(kCatalog.size() == 8);
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
