#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <auth/jwt-service.hxx>
#include <config/config-service.hxx>
#include <doctest/doctest.h>
#include <set>
#include <string>

namespace
{

void armSecrets()
{
  ConfigService::setRuntimeString("jwt.secret", std::string(40, 'a'));
  ConfigService::setRuntimeString("jwt.refresh_secret", std::string(40, 'b'));
  ConfigService::setRuntimeString("jwt.access_ttl_minutes", "15");
  ConfigService::setRuntimeString("jwt.refresh_ttl_days", "30");
}

}

TEST_CASE("every token minted for one subject is distinct")
{
  armSecrets();
  const JwtService service;

  std::set<std::string> tokens;
  std::set<std::string> identifiers;
  for (int i = 0; i < 500; ++i) {
    const std::string token = service.generateAccess({{"sub", "1"}});
    tokens.insert(token);
    const auto claims = service.verifyAccess(token);
    const auto identifier = claims.find("jti");
    REQUIRE(identifier != claims.end());
    CHECK(identifier->second.size() == 32);
    identifiers.insert(identifier->second);
  }
  CHECK(tokens.size() == 500);
  CHECK(identifiers.size() == 500);
}

TEST_CASE("the refresh token of one subject is distinct too")
{
  armSecrets();
  const JwtService service;

  CHECK(service.generateRefresh({{"sub", "1"}}) !=
        service.generateRefresh({{"sub", "1"}}));
}

TEST_CASE("a caller keeps the identifier it supplies")
{
  armSecrets();
  const JwtService service;

  const std::string token =
      service.generateAccess({{"sub", "1"}, {"jti", "caller-supplied"}});
  CHECK(service.verifyAccess(token).at("jti") == "caller-supplied");
}
