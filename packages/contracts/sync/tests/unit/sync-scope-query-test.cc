#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sync/sync-filter.hxx>
#include <sync/sync-limits.hxx>

#include <string>
#include <vector>

namespace
{
constexpr std::string_view kHead = "SELECT * FROM t WHERE parent IN (";
constexpr std::string_view kAfter = " AND (created_at > ? OR (created_at = ? AND id > ?))";
constexpr std::string_view kTail = " AND owner = ? ORDER BY created_at, id LIMIT 200";
}

TEST_CASE("a scoped pull names one placeholder per scope id")
{
  SyncFilter filter;
  filter.scopeIds = {7, 9, 11};
  const auto parts = sync_query::buildScopedQuery(
      {.filter = filter, .head = kHead, .after = kAfter, .tail = kTail});

  CHECK(parts.query ==
        "SELECT * FROM t WHERE parent IN (?,?,?) AND owner = ? ORDER BY created_at, id LIMIT 200");
  CHECK(parts.args == std::vector<std::string>{"7", "9", "11"});
}

TEST_CASE("a scoped page continues after the last row it was given")
{
  SyncFilter filter;
  filter.scopeIds = {4};
  filter.startTime = 1700000000;
  filter.startId = 31;
  const auto parts = sync_query::buildScopedQuery(
      {.filter = filter, .head = kHead, .after = kAfter, .tail = kTail});

  CHECK(parts.query ==
        "SELECT * FROM t WHERE parent IN (?) AND (created_at > ? OR (created_at = ? AND id > ?))"
        " AND owner = ? ORDER BY created_at, id LIMIT 200");
  CHECK(parts.args == std::vector<std::string>{"4", "1700000000", "1700000000", "31"});
}

TEST_CASE("a start time without an id starts at that second")
{
  SyncFilter filter;
  filter.scopeIds = {4};
  filter.startTime = 1700000000;
  const auto parts = sync_query::buildScopedQuery(
      {.filter = filter, .head = kHead, .after = kAfter, .tail = kTail});

  CHECK(parts.args == std::vector<std::string>{"4", "1700000000", "1700000000", "0"});
}

TEST_CASE("the user placeholders follow the scope and the range")
{
  SyncFilter filter;
  filter.scopeIds = {4, 5};
  filter.userId = 12;
  const auto parts = sync_query::withUser(
      {.parts = sync_query::buildScopedQuery(
           {.filter = filter, .head = kHead, .after = kAfter, .tail = kTail}),
       .userId = filter.userId,
       .placeholders = 1});

  CHECK(parts.args == std::vector<std::string>{"4", "5", "12"});
  CHECK(SyncLimits::kMaxScopeIds == 50);
}
