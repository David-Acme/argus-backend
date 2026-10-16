#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/refusal-log.hxx>

TEST_CASE("an expired verdict is named expired")
{
  CHECK(auth_refusal::reasonOfVerdict("token expired") == "expired");
}

TEST_CASE("a verdict about the device is named device-mismatch")
{
  CHECK(auth_refusal::reasonOfVerdict("device does not match the session") == "device-mismatch");
}

TEST_CASE("any other verdict, or none, is named invalid")
{
  CHECK(auth_refusal::reasonOfVerdict("session revoked") == "invalid");
  CHECK(auth_refusal::reasonOfVerdict("") == "invalid");
}

TEST_CASE("the refusal line names the path, the status and the reason, and no token")
{
  const std::string logged = auth_refusal::line("/rtc/token", 401, "expired");
  CHECK(logged == "auth refused: /rtc/token status 401 reason expired");
  CHECK(logged.find("eyJ") == std::string::npos);
}
