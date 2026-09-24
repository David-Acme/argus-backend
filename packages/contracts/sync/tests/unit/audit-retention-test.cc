#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sync/audit-retention.hxx>
#include <sync/sync-errors.hxx>

#include <string>

TEST_CASE("the audit window defaults to ninety days")
{
  CHECK(audit_retention::kDefaultDays == 90);
  CHECK(audit_retention::kDefaultDays > 0);
}

TEST_CASE("the window's refusal is the 409 the app re-bootstraps on")
{
  CHECK(SyncErrors::ReplicaTooOld.status == 409);
  CHECK(std::string(SyncErrors::ReplicaTooOld.wireCode()) ==
        std::string(toString(ErrorCode::Conflict)));
}
