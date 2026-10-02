#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <sqlite/transaction.hxx>

#include <optional>

TEST_CASE("a commit reaches every live observer and none that has gone")
{
  int first = 0;
  int second = 0;
  db_transaction::CommitObserver kept([&first] { ++first; });
  {
    std::optional<db_transaction::CommitObserver> scoped;
    scoped.emplace([&second] { ++second; });
    db_transaction::CommitObserver::notifyAll();
  }
  db_transaction::CommitObserver::notifyAll();

  CHECK(first == 2);
  CHECK(second == 1);
}
