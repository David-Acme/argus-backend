#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>
#include <errors/response-exception.hxx>

#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace
{
struct CodeName
{
  ErrorCode code;
  const char* name;
};
}

TEST_CASE("every error code has exactly one wire string")
{
  const std::vector<CodeName> table = {
      {.code = ErrorCode::Error, .name = "ERROR"},
      {.code = ErrorCode::BadRequest, .name = "BAD_REQUEST"},
      {.code = ErrorCode::Unauthorized, .name = "UNAUTHORIZED"},
      {.code = ErrorCode::Forbidden, .name = "FORBIDDEN"},
      {.code = ErrorCode::NotFound, .name = "NOT_FOUND"},
      {.code = ErrorCode::MethodNotAllowed, .name = "METHOD_NOT_ALLOWED"},
      {.code = ErrorCode::Conflict, .name = "CONFLICT"},
      {.code = ErrorCode::RemoteNotAllowed, .name = "REMOTE_NOT_ALLOWED"},
      {.code = ErrorCode::ServiceUnavailable, .name = "SERVICE_UNAVAILABLE"},
      {.code = ErrorCode::TooManyRequests, .name = "TOO_MANY_REQUESTS"},
      {.code = ErrorCode::InternalError, .name = "INTERNAL_ERROR"},
      {.code = ErrorCode::BadGateway, .name = "BAD_GATEWAY"},
      {.code = ErrorCode::ValidationError, .name = "VALIDATION_ERROR"},
      {.code = ErrorCode::UserNotFound, .name = "USER_NOT_FOUND"},
      {.code = ErrorCode::TtsNotLoaded, .name = "TTS_NOT_LOADED"},
      {.code = ErrorCode::SttNotLoaded, .name = "STT_NOT_LOADED"},
      {.code = ErrorCode::LlmNotLoaded, .name = "LLM_NOT_LOADED"},
      {.code = ErrorCode::VlmNotLoaded, .name = "VLM_NOT_LOADED"},
      {.code = ErrorCode::CameraUnreachable, .name = "CAMERA_UNREACHABLE"},
      {.code = ErrorCode::Cancelled, .name = "CANCELLED"},
      {.code = ErrorCode::DeadlineExceeded, .name = "DEADLINE_EXCEEDED"},
      {.code = ErrorCode::SessionNotFound, .name = "SESSION_NOT_FOUND"},
      {.code = ErrorCode::AccountDisabled, .name = "ACCOUNT_DISABLED"},
      {.code = ErrorCode::RtcUnavailable, .name = "RTC_UNAVAILABLE"},
      {.code = ErrorCode::CallNotFound, .name = "CALL_NOT_FOUND"},
      {.code = ErrorCode::CallTaken, .name = "CALL_TAKEN"},
      {.code = ErrorCode::CallExpired, .name = "CALL_EXPIRED"}};

  CHECK(table.size() ==
        static_cast<std::size_t>(ErrorCode::CallExpired) + 1);

  for (const auto& row : table)
    CHECK(std::string(toString(row.code)) == row.name);
}

TEST_CASE("a value outside the enum throws instead of formatting")
{
  CHECK_THROWS_AS(toString(static_cast<ErrorCode>(200)),
                  std::invalid_argument);
}

TEST_CASE("a bare message is a 400 under the generic code")
{
  const ResponseException error("boom");

  CHECK(error.statusCode() == 400);
  CHECK(error.errorCode() == "ERROR");
  CHECK(std::string(error.what()) == "boom");

  const auto& carried = std::get<ResponseError>(error.errors());
  CHECK(carried.code == "ERROR");
  CHECK(carried.message == "boom");
}

TEST_CASE("a definition carries its own code, status and message")
{
  const ResponseException error(
      ErrorDefinition{ErrorCode::UserNotFound, 404, "no such user"});

  CHECK(error.statusCode() == 404);
  CHECK(error.errorCode() == "USER_NOT_FOUND");
  CHECK(std::string(error.what()) == "no such user");

  const auto& carried = std::get<ResponseError>(error.errors());
  CHECK(carried.code == "USER_NOT_FOUND");
  CHECK(carried.message == "no such user");
}

TEST_CASE("withMessage keeps the catalog's code and status")
{
  const ErrorDefinition definition{ErrorCode::BadGateway, 502,
                                   "Service unreachable"};
  const auto withDetail = definition.withMessage("camera said no");

  CHECK(std::string(toString(withDetail.code)) == "BAD_GATEWAY");
  CHECK(withDetail.status == definition.status);
  CHECK(std::string(withDetail.message) == "camera said no");
  CHECK(std::string(definition.message) == "Service unreachable");

  const ResponseException error(withDetail);
  CHECK(error.statusCode() == 502);
  CHECK(error.errorCode() == "BAD_GATEWAY");
  CHECK(std::string(error.what()) == "camera said no");
}

TEST_CASE("a definition outlives the text a caller handed over")
{
  std::string message = "Temporary message";
  const ErrorDefinition definition{ErrorCode::BadRequest, 400, message};
  const ResponseException error(definition);
  message.assign("Changed message");

  CHECK(std::string(error.what()) == "Temporary message");
  CHECK(error.statusCode() == 400);
}

TEST_CASE("a list of errors keeps every entry and reports the first code")
{
  const ResponseException error(
      422,
      std::vector<ResponseError>{{"EMAIL", "invalid"}, {"NAME", "empty"}});

  CHECK(error.statusCode() == 422);
  CHECK(error.errorCode() == "EMAIL");
  CHECK(std::string(error.what()) == "invalid");

  const auto& carried = std::get<std::vector<ResponseError>>(error.errors());
  REQUIRE(carried.size() == 2);
  CHECK(carried[0].code == "EMAIL");
  CHECK(carried[1].code == "NAME");
  CHECK(carried[1].message == "empty");
}

TEST_CASE("an empty error list is rejected at construction")
{
  CHECK_THROWS_AS(ResponseException(422, std::vector<ResponseError>{}),
                  std::invalid_argument);
}
