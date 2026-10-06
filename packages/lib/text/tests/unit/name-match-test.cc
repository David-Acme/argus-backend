#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <text/name-match.hxx>

#include <string>
#include <vector>

using text_norm::matchName;
using text_norm::NameMatchKind;

namespace
{
std::vector<std::string> names()
{
  return {"Garaje", "Patio trasero", "Patio delantero", "Cámara de la sala"};
}
}

TEST_CASE("folding drops accents, case and punctuation")
{
  CHECK(text_norm::folded("  CÁMARA, de la   Sala! ") == "camara de la sala");
  CHECK(text_norm::folded("") == "");
}

TEST_CASE("a whole name matches whatever its case and accents")
{
  const auto match = matchName(names(), "CAMARA de la SALA");
  CHECK(match.kind == NameMatchKind::Exact);
  REQUIRE(match.hits.size() == 1);
  CHECK(match.hits.front() == 3);
}

TEST_CASE("a word only one name holds matches that name")
{
  const auto match = matchName(names(), "trasero");
  CHECK(match.kind == NameMatchKind::Exact);
  REQUIRE(match.hits.size() == 1);
  CHECK(match.hits.front() == 1);
}

TEST_CASE("a word several names hold is ambiguous and lists them in order")
{
  const auto match = matchName(names(), "patio");
  CHECK(match.kind == NameMatchKind::Ambiguous);
  CHECK(match.hits == std::vector<std::size_t>{1, 2});
}

TEST_CASE("an exact name wins over a longer name that holds its words")
{
  const std::vector<std::string> names{"Patio", "Patio trasero"};
  const auto match = matchName(names, "patio");
  CHECK(match.kind == NameMatchKind::Exact);
  CHECK(match.hits == std::vector<std::size_t>{0});
}

TEST_CASE("nothing, blanks or a name nobody has is missing")
{
  CHECK(matchName(names(), "").kind == NameMatchKind::Missing);
  CHECK(matchName(names(), "  ,, ").kind == NameMatchKind::Missing);
  CHECK(matchName(names(), "jardín").kind == NameMatchKind::Missing);
  CHECK(matchName(names(), "el garaje").kind == NameMatchKind::Missing);
  CHECK(matchName({}, "garaje").kind == NameMatchKind::Missing);
}
