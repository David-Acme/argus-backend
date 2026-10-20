#include <doctest/doctest.h>

#include <feature/messenger/services/relay-recipient.hxx>

#include <vector>

namespace
{

const std::vector<relay_recipient::Member>& roster()
{
  static const std::vector<relay_recipient::Member> members{
      {.userId = 1, .name = "David Acme"},
      {.userId = 2, .name = "Ana Lopez"},
      {.userId = 3, .name = "Ana Maria Ruiz"},
      {.userId = 4, .name = "Lucia"},
  };
  return members;
}

relay_recipient::Resolution resolveFor(std::string_view asked, int64_t senderId = 99)
{
  return relay_recipient::resolve({.roster = roster(), .asked = asked, .senderId = senderId});
}

}

TEST_CASE("a one-word name that only one member holds resolves to that member")
{
  const auto resolution = resolveFor("David");
  CHECK(resolution.kind == relay_recipient::ResolutionKind::Resolved);
  CHECK(resolution.userId == 1);
  CHECK(resolution.candidates.empty());
}

TEST_CASE("a name held by two members asks, and lists both")
{
  const auto resolution = resolveFor("ana");
  CHECK(resolution.kind == relay_recipient::ResolutionKind::Ambiguous);
  REQUIRE(resolution.candidates.size() == 2);
  CHECK(resolution.candidates[0] == 2);
  CHECK(resolution.candidates[1] == 3);
}

TEST_CASE("a full name picks the one member among the ones sharing its first word")
{
  CHECK(resolveFor("ana lopez").userId == 2);
  CHECK(resolveFor("ana maria").userId == 3);
}

TEST_CASE("an accent in the spoken name still matches the stored one")
{
  const auto resolution = resolveFor("Lucía");
  CHECK(resolution.kind == relay_recipient::ResolutionKind::Resolved);
  CHECK(resolution.userId == 4);
}

TEST_CASE("a name nobody holds, and an empty name, are missing")
{
  CHECK(resolveFor("Pedro").kind == relay_recipient::ResolutionKind::Missing);
  CHECK(resolveFor("").kind == relay_recipient::ResolutionKind::Missing);
  CHECK(resolveFor("   ").kind == relay_recipient::ResolutionKind::Missing);
}

TEST_CASE("a relay to the sender's own name is refused as self, never sent")
{
  const auto resolution = resolveFor("David", 1);
  CHECK(resolution.kind == relay_recipient::ResolutionKind::Self);
  CHECK(resolution.userId == 1);
}
