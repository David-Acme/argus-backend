#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <text/text-norm.hxx>

#include <string>

TEST_CASE("a decomposed spelling normalises to its composed twin")
{
  const std::string composed = "caf\u00E9 ma\u00F1ana cig\u00FC\u00F1a";
  const std::string decomposed = "cafe\u0301 man\u0303ana cig\u00FCn\u0303a";
  CHECK(composed != decomposed);
  CHECK(text_norm::nfc(composed) == composed);
  CHECK(text_norm::nfc(decomposed) == composed);
}

TEST_CASE("normalisation is idempotent and leaves ascii and empty text alone")
{
  CHECK(text_norm::nfc("") == "");
  CHECK(text_norm::nfc("hola que tal") == "hola que tal");
  CHECK(text_norm::nfc("a\u0300\u0301") == "\u00E0\u0301");
  CHECK(text_norm::nfc(text_norm::nfc("a\u0300\u0301")) == "\u00E0\u0301");
}

TEST_CASE("a combining mark is composed onto the letter it belongs to")
{
  CHECK(text_norm::nfc("PINGU\u0308INO") == "PING\u00DCINO");
  CHECK(text_norm::nfc("\u00BFqu\u00E9 tengo en la agenda?") == "\u00BFqu\u00E9 tengo en la agenda?");
}
