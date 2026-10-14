#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/auth-access.hxx>
#include <auth/details/identity-access.hxx>
#include <config/config-service.hxx>

#include <stdexcept>

TEST_CASE("an empty auth.target is a configuration error, never a plaintext dial")
{
  ConfigService::setRuntimeString("auth.target", "");
  CHECK_THROWS_WITH_AS(requireAuthTarget(), "auth.target is empty; run scripts/setup.sh",
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(filterAuthClient(), "auth.target is empty; run scripts/setup.sh",
                       std::runtime_error);
  ConfigService::setRuntimeString("auth.target", "127.0.0.1:1");
  CHECK_NOTHROW(requireAuthTarget());
  CHECK(filterAuthClient() != nullptr);
}

TEST_CASE("an empty identity.target is a configuration error, never a plaintext dial")
{
  ConfigService::setRuntimeString("identity.target", "");
  CHECK_THROWS_WITH_AS(requireIdentityTarget(), "identity.target is empty; run scripts/setup.sh",
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(filterIdentityClient(), "identity.target is empty; run scripts/setup.sh",
                       std::runtime_error);
  ConfigService::setRuntimeString("identity.target", "127.0.0.1:1");
  CHECK_NOTHROW(requireIdentityTarget());
  CHECK(filterIdentityClient() != nullptr);
}
