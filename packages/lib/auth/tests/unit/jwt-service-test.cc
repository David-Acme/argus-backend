#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <auth/jwt-service.hxx>
#include <config/config-service.hxx>
#include <doctest/doctest.h>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>

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
  const JwtService service{JwtRole::Issuer};

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
  const JwtService service{JwtRole::Issuer};

  CHECK(service.generateRefresh({{"sub", "1"}}) !=
        service.generateRefresh({{"sub", "1"}}));
}

TEST_CASE("a caller keeps the identifier it supplies")
{
  armSecrets();
  const JwtService service{JwtRole::Issuer};

  const std::string token =
      service.generateAccess({{"sub", "1"}, {"jti", "caller-supplied"}});
  CHECK(service.verifyAccess(token).at("jti") == "caller-supplied");
}

TEST_CASE("a JwtService names its role: no default silently makes a verifier an issuer")
{
  CHECK_FALSE(std::is_default_constructible_v<JwtService>);
  CHECK_FALSE(std::is_convertible_v<JwtRole, JwtService>);
  CHECK(std::is_constructible_v<JwtService, JwtRole>);
}

TEST_CASE("an issuer refuses equal access and refresh secrets")
{
  armSecrets();
  ConfigService::setRuntimeString("jwt.refresh_secret", std::string(40, 'a'));
  CHECK_THROWS_AS(JwtService{JwtRole::Issuer}, std::runtime_error);
  CHECK_NOTHROW(JwtService{JwtRole::Verifier});
  armSecrets();
}

TEST_CASE("a refresh token never verifies as an access token, nor the reverse")
{
  armSecrets();
  const JwtService service{JwtRole::Issuer};

  const std::string access = service.generateAccess({{"sub", "1"}});
  const std::string refresh = service.generateRefresh({{"sub", "1"}, {"sid", "s"}});
  CHECK(service.verifyAccess(access).at("typ") == "access");
  CHECK(service.verifyRefresh(refresh).at("typ") == "refresh");
  CHECK(service.verifyAccess(refresh).empty());
  CHECK(service.verifyRefresh(access).empty());

  const std::string untypedAccess = service.generate(
      {.claims = {{"sub", "1"}}, .secret = std::string(40, 'a'), .expiresInSeconds = 60});
  CHECK(service.verifyAccess(untypedAccess).empty());

  const std::string legacyRefresh = service.generate(
      {.claims = {{"sub", "1"}}, .secret = std::string(40, 'b'), .expiresInSeconds = 60});
  CHECK(service.verifyRefresh(legacyRefresh).at("sub") == "1");

  const std::string forgedType = service.generate(
      {.claims = {{"sub", "1"}, {"typ", "access"}}, .secret = std::string(40, 'b'), .expiresInSeconds = 60});
  CHECK(service.verifyRefresh(forgedType).empty());
}

TEST_CASE("a verifier loads the access secret alone and can neither verify nor mint a refresh token")
{
  armSecrets();
  const JwtService issuer{JwtRole::Issuer};
  const std::string refresh = issuer.generateRefresh({{"sub", "1"}});

  ConfigService::setRuntimeString("jwt.refresh_secret", "");
  const JwtService verifier{JwtRole::Verifier};
  CHECK(verifier.verifyAccess(issuer.generateAccess({{"sub", "2"}})).at("sub") == "2");
  CHECK(verifier.verifyRefresh(refresh).empty());
  CHECK_THROWS(verifier.generateRefresh({{"sub", "1"}}));
  CHECK_THROWS_AS(JwtService{JwtRole::Issuer}, std::runtime_error);
  armSecrets();
}
