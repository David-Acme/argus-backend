#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <errors/validation-exception.hxx>
#include <feature/invitation/dtos/create-invitation-dto.hxx>
#include <feature/user/dtos/update-user-dto.hxx>
#include <json/reader.h>

#include <sstream>
#include <string>

namespace
{
Json::Value parse(const std::string& text)
{
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  std::istringstream stream(text);
  REQUIRE(Json::parseFromStream(builder, stream, &root, &errors));
  return root;
}
}

TEST_CASE("an invitation names a non-owner role of this build, and has no default one")
{
  for (const auto* role : {"resident", "guard", "guest"}) {
    CAPTURE(role);
    const auto dto = CreateInvitationDto::fromJson(parse(std::string(R"({"role":")") + role + "\"}"));
    CHECK(userRoleToString(dto.userRole) == role);
  }
  for (const auto* body : {"{}", R"({"role":""})", R"({"role":"owner"})", R"({"role":"unknown"})",
                           R"({"role":"agronomist"})", R"({"role":"Guard"})", R"({"role":7})"}) {
    CAPTURE(body);
    CHECK_THROWS_AS(CreateInvitationDto::fromJson(parse(body)), ValidationException);
  }
}

TEST_CASE("a role update names a role of this build, never the placeholder of an unknown one")
{
  for (const auto* role : {"owner", "resident", "guard", "guest"}) {
    CAPTURE(role);
    const auto dto = UpdateUserDto::fromJson(parse(std::string(R"({"role":")") + role + "\"}"));
    REQUIRE(dto.userRole.has_value());
    CHECK(userRoleToString(dto.userRole.value_or(UserRole::Unknown)) == role);
  }
  for (const auto* body : {R"({"role":"unknown"})", R"({"role":"agronomist"})", R"({"role":""})",
                           R"({"role":"OWNER"})"}) {
    CAPTURE(body);
    CHECK_THROWS_AS(UpdateUserDto::fromJson(parse(body)), ValidationException);
  }
  CHECK_THROWS_AS(UpdateUserDto::fromJson(parse("{}")), ValidationException);
}
