#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <mcp/confirmation.hxx>

#include <chrono>
#include <set>
#include <string>

using namespace argus::mcp;
using namespace std::chrono_literals;

namespace
{
struct Clock
{
  std::chrono::steady_clock::time_point at{std::chrono::steady_clock::time_point{} + 1000s};
  ConfirmationLedger::Clock reader() { return [this] { return at; }; }
};

ConfirmationKey keyFor(int64_t user, const std::string& tool = "modules.disable", const std::string& target = "surveillance")
{
  return {.userId = user, .tool = tool, .target = target};
}
}

TEST_CASE("a token is short, lowercase hex and works once")
{
  ConfirmationLedger ledger;
  const std::string token = ledger.issue(keyFor(7));
  CHECK(token.size() == 6);
  CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
  CHECK(ledger.consume(keyFor(7), token));
  CHECK_FALSE(ledger.consume(keyFor(7), token));
}

TEST_CASE("a token is bound to the user, the tool and the target it was issued for")
{
  ConfirmationLedger ledger;
  const std::string token = ledger.issue(keyFor(7));
  CHECK_FALSE(ledger.consume(keyFor(8), token));
  CHECK_FALSE(ledger.consume(keyFor(7, "calendar.cancel_event"), token));
  CHECK_FALSE(ledger.consume(keyFor(7, "modules.disable", "productivity"), token));
  CHECK(ledger.consume(keyFor(7), token));
}

TEST_CASE("a wrong token does not spend the right one")
{
  ConfirmationLedger ledger;
  const std::string token = ledger.issue(keyFor(7));
  CHECK_FALSE(ledger.consume(keyFor(7), "000000"));
  CHECK_FALSE(ledger.consume(keyFor(7), ""));
  CHECK(ledger.consume(keyFor(7), token));
}

TEST_CASE("a token expires after its lifetime and is forgotten")
{
  Clock clock;
  ConfirmationLedger ledger({.lifetime = 120s, .capacity = 8}, clock.reader());
  const std::string token = ledger.issue(keyFor(7));
  clock.at += 119s;
  CHECK(ledger.pending() == 1);
  clock.at += 2s;
  CHECK_FALSE(ledger.consume(keyFor(7), token));
  CHECK(ledger.pending() == 0);
}

TEST_CASE("issuing again for the same key replaces the earlier token")
{
  Clock clock;
  ConfirmationLedger ledger({.lifetime = 120s, .capacity = 8}, clock.reader());
  const std::string first = ledger.issue(keyFor(7));
  std::string second = ledger.issue(keyFor(7));
  while (second == first)
    second = ledger.issue(keyFor(7));
  CHECK(ledger.pending() == 1);
  CHECK_FALSE(ledger.consume(keyFor(7), first));
  CHECK(ledger.consume(keyFor(7), second));
}

TEST_CASE("the ledger stays inside its capacity by dropping the oldest")
{
  Clock clock;
  ConfirmationLedger ledger({.lifetime = 1000s, .capacity = 3}, clock.reader());
  const std::string oldest = ledger.issue(keyFor(1));
  clock.at += 1s;
  (void)ledger.issue(keyFor(2));
  clock.at += 1s;
  (void)ledger.issue(keyFor(3));
  clock.at += 1s;
  const std::string newest = ledger.issue(keyFor(4));
  CHECK(ledger.pending() <= 3);
  CHECK_FALSE(ledger.consume(keyFor(1), oldest));
  CHECK(ledger.consume(keyFor(4), newest));
}

TEST_CASE("tokens are not all the same")
{
  ConfirmationLedger ledger;
  std::set<std::string> seen;
  for (int user = 0; user < 64; ++user)
    seen.insert(ledger.issue(keyFor(user)));
  CHECK(seen.size() > 32);
}
