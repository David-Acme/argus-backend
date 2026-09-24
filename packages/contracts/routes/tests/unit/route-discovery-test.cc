#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <routes/service-discovery.hxx>
#include <string_view>

namespace
{

bool isLowerAlpha(char character)
{
  return character >= 'a' && character <= 'z';
}

bool isLowerAlphaNumeric(char character)
{
  return isLowerAlpha(character) || (character >= '0' && character <= '9');
}

bool isServiceTypeCharacter(char character)
{
  return isLowerAlphaNumeric(character) || character == '-' ||
         character == '_' || character == '.';
}

}

TEST_CASE("the announced spellings are pinned")
{
  CHECK(routes::kServiceType == "_argus-route._tcp");
  CHECK(routes::kTxtPath == "path");
  CHECK(routes::kTxtHttps == "https");
}

TEST_CASE("the service type is a DNS-SD name")
{
  const std::string_view type = routes::kServiceType;

  CHECK(type.front() == '_');
  CHECK(type.find('.') != std::string_view::npos);
  CHECK(type.substr(type.rfind('.')) == "._tcp");

  for (const char character : type)
    CHECK(isServiceTypeCharacter(character));

  CHECK(type.rfind("..") == std::string_view::npos);
  CHECK(type.front() != '.');
  CHECK(type.back() != '.');
}

TEST_CASE("the TXT keys are plain alphanumeric")
{
  for (const std::string_view key : {routes::kTxtPath, routes::kTxtHttps}) {
    CHECK(!key.empty());
    CHECK(key.size() <= 9);
    for (const char character : key)
      CHECK(isLowerAlphaNumeric(character));
  }
}
