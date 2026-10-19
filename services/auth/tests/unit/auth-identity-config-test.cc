#include <doctest/doctest.h>

#include <config/auth-config.hxx>
#include <config/config-service.hxx>

#include <stdexcept>

TEST_CASE("an empty identity.target is a configuration error, never a plaintext dial")
{
  ConfigService::setRuntimeString("identity.target", "");
  CHECK_THROWS_WITH_AS(static_cast<void>(AuthConfig::resolveIdentity()),
                       "identity.target is empty; run scripts/setup.sh", std::runtime_error);
  ConfigService::setRuntimeString("identity.target", "tls:127.0.0.1:7240");
  CHECK(AuthConfig::resolveIdentity().target == "tls:127.0.0.1:7240");
}
