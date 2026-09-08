#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/contracts/sync-filter.hxx>
#include <shared/repositories/event/event-repository.hxx>
#include <shared/services/sqlite/db-service.hxx>

TEST_CASE("event sync serves empty with no read-only client installed")
{
  DbService::setReadOnlyClient(nullptr);

  const EventRepository repository;
  const SyncFilter filter;

  const auto created = drogon::sync_wait(repository.find(filter));
  CHECK(created.empty());
  const auto deleted = drogon::sync_wait(repository.findDeleted(filter));
  CHECK(deleted.empty());
  CHECK_FALSE(drogon::sync_wait(repository.findLast(filter)).has_value());
  CHECK_FALSE(
      drogon::sync_wait(repository.findLastDeleted(filter)).has_value());
}
