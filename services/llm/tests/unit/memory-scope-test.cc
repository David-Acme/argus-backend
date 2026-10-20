#include <doctest/doctest.h>
#include <shared/vocabulary/memory-scope.hxx>
#include <test-support/value-of.hxx>

TEST_CASE("personal facts keep the stored spelling of the scope column")
{
  CHECK(memoryScopeToString(MemoryScope::Personal) == "user");
  CHECK(memoryScopeFromString("user") == MemoryScope::Personal);
}

TEST_CASE("household facts carry the household spelling")
{
  CHECK(memoryScopeToString(MemoryScope::Household) == "household");
  CHECK(memoryScopeFromString("household") == MemoryScope::Household);
}

TEST_CASE("every scope round trips through its name")
{
  for (const MemoryScope scope : {MemoryScope::Personal, MemoryScope::Household}) {
    const auto parsed = valueOf(memoryScopeFromString(memoryScopeToString(scope)));
    CHECK(parsed == scope);
  }
}

TEST_CASE("a scope name outside the enum answers nothing")
{
  CHECK_FALSE(memoryScopeFromString("global").has_value());
  CHECK_FALSE(memoryScopeFromString("User").has_value());
  CHECK_FALSE(memoryScopeFromString("").has_value());
}
