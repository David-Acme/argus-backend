#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/contracts/sync-filter.hxx>
#include <shared/repositories/event/event-repository.hxx>
#include <shared/services/sqlite/db-service.hxx>

// argus.db retirement (F6-4): the event sync tables have no owner database
// anymore, so an uninstalled read-only client must serve the empty shape
// instead of falling back to a database that lacks the tables (which 500s).
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
