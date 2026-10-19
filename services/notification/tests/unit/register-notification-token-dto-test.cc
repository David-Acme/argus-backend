#include <doctest/doctest.h>

#include <feature/notification/dtos/register-notification-token-dto.hxx>

#include <json/value.h>

#include <string>

namespace
{
Json::Value registrationWithToken(std::size_t length)
{
  Json::Value body(Json::objectValue);
  body["token"] = std::string(length, 't');
  body["platform"] = "android";
  body["lang"] = "es";
  return body;
}
}

TEST_CASE("a registered token of 255 characters is accepted")
{
  CHECK_NOTHROW(RegisterNotificationTokenDto::fromJson(registrationWithToken(255)));
  CHECK(RegisterNotificationTokenDto::fromJson(registrationWithToken(255)).token.size() == 255);
}

TEST_CASE("a registered token of 256 characters is refused by the validation rules")
{
  CHECK_THROWS(RegisterNotificationTokenDto::fromJson(registrationWithToken(256)));
}
