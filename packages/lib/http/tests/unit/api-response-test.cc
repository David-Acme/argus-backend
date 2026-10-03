#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <http/api-response.hxx>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/details/http-errors.hxx>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <json/json.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
Json::Value bodyOf(const drogon::HttpResponsePtr& response)
{
  Json::Value parsed;
  Json::CharReaderBuilder builder;
  std::istringstream stream{std::string(response->getBody())};
  std::string error;
  CHECK(Json::parseFromStream(builder, stream, &parsed, &error));
  return parsed;
}

Json::Value sampleData()
{
  Json::Value data;
  data["id"] = 7;
  data["name"] = "Alice";
  return data;
}

drogon::HttpResponsePtr answerTo(const std::exception& error)
{
  drogon::HttpResponsePtr response;
  ErrorHandler::handleException(error, drogon::HttpRequest::newHttpRequest(),
                                [&](const auto& result) {
    response = result;
  });
  return response;
}
}

TEST_CASE("ApiResponse::ok wraps data in the status/info/errors envelope")
{
  const auto response = ApiResponse::ok(sampleData());
  CHECK(response->getStatusCode() == drogon::k200OK);
  CHECK(response->getContentType() == drogon::CT_APPLICATION_JSON);

  const auto body = bodyOf(response);
  CHECK(body["status"] == 200);
  CHECK(body["info"]["id"] == 7);
  CHECK(body["info"]["name"] == "Alice");
  CHECK(body["errors"].isNull());
}

TEST_CASE("ApiResponse::ok without data keeps info null")
{
  const auto body = bodyOf(ApiResponse::ok());
  CHECK(body["status"] == 200);
  CHECK(body["info"].isNull());
  CHECK(body["errors"].isNull());
}

TEST_CASE("ApiResponse::created returns 201 with the payload")
{
  const auto response = ApiResponse::created(sampleData());
  CHECK(response->getStatusCode() == drogon::k201Created);

  const auto body = bodyOf(response);
  CHECK(body["status"] == 201);
  CHECK(body["info"]["id"] == 7);
  CHECK(body["errors"].isNull());
}

TEST_CASE("ApiResponse::noContent keeps info and errors null")
{
  const auto response = ApiResponse::noContent();
  CHECK(response->getStatusCode() == drogon::k204NoContent);

  const auto body = bodyOf(response);
  CHECK(body["status"] == 204);
  CHECK(body["info"].isNull());
  CHECK(body["errors"].isNull());
}

TEST_CASE("ApiResponse::error carries the status, code and message")
{
  const auto response =
      ApiResponse::error({.statusCode = 404,
                          .errorCode = "NOT_FOUND",
                          .message = "Path not found"});
  CHECK(response->getStatusCode() == drogon::k404NotFound);

  const auto body = bodyOf(response);
  CHECK(body["status"] == 404);
  CHECK(body["info"].isNull());
  CHECK(body["errors"]["code"] == "NOT_FOUND");
  CHECK(body["errors"]["message"] == "Path not found");
}

TEST_CASE("ApiResponse::error formats a catalog definition as it stands")
{
  const auto response = ApiResponse::error(HttpErrors::MethodNotAllowed);
  CHECK(response->getStatusCode() == drogon::k405MethodNotAllowed);

  const auto body = bodyOf(response);
  CHECK(body["status"] == 405);
  CHECK(body["errors"]["code"] == "METHOD_NOT_ALLOWED");
  CHECK(body["errors"]["message"] == "Method not allowed");
}

TEST_CASE("ApiResponse::error serializes single and array errors")
{
  const auto single = bodyOf(ApiResponse::error(ResponseException(
      503, ErrorDefinition{.code = ErrorCode::ServiceUnavailable,
                           .status = 503,
                           .message = "Engine is not loaded"})));
  CHECK(single["status"] == 503);
  CHECK(single["errors"]["code"] == "SERVICE_UNAVAILABLE");
  CHECK(single["errors"]["message"] == "Engine is not loaded");

  const auto multiple = bodyOf(ApiResponse::error(ResponseException(
      422, std::vector<ResponseError>{
               {.code = "UNSUPPORTED_LANGUAGE",
                .message = "Unsupported language"},
               {.code = "VOICE_UNKNOWN", .message = "Unknown voice"}})));
  CHECK(multiple["status"] == 422);
  CHECK(multiple["errors"].isArray());
  CHECK(multiple["errors"][0]["code"] == "UNSUPPORTED_LANGUAGE");
  CHECK(multiple["errors"][1]["message"] == "Unknown voice");
}

TEST_CASE("the advice preserves a definition's code, status and text")
{
  const auto body = bodyOf(answerTo(ResponseException(
      ErrorDefinition{.code = ErrorCode::NotFound,
                      .status = 404,
                      .message = "Camera not found"})));
  CHECK(body["status"] == 404);
  CHECK(body["info"].isNull());
  CHECK(body["errors"]["code"] == "NOT_FOUND");
  CHECK(body["errors"]["message"] == "Camera not found");
}

TEST_CASE("the advice preserves error arrays at the HTTP boundary")
{
  const ResponseException error(422, std::vector<ResponseError>{
                                        {.code = "FIRST",
                                         .message = "First failure"},
                                        {.code = "SECOND",
                                         .message = "Second failure"}});
  const auto response = answerTo(error);
  REQUIRE(response);
  CHECK(response->getStatusCode() == drogon::k422UnprocessableEntity);

  const auto body = bodyOf(response);
  CHECK(body["errors"].isArray());
  CHECK(body["errors"].size() == 2);
  CHECK(body["info"].isNull());
}

TEST_CASE("the advice turns an untyped exception into a 500 that keeps its words in the log")
{
  const auto response = answerTo(std::runtime_error("a broken invariant"));
  CHECK(response->getStatusCode() == drogon::k500InternalServerError);

  const auto body = bodyOf(response);
  CHECK(body["status"] == 500);
  CHECK(body["errors"]["code"] == "INTERNAL_ERROR");
  CHECK(body["errors"]["message"] == "Internal error");
}

TEST_CASE("the advice turns a validation failure into 422 field errors")
{
  const ValidationException failure(
      ValidationErrors{{"email", {"email must be a valid email"}}});
  const auto response = answerTo(failure);
  CHECK(response->getStatusCode() == drogon::k422UnprocessableEntity);

  const auto body = bodyOf(response);
  CHECK(body["status"] == 422);
  CHECK(body["errors"]["code"] == "VALIDATION_ERROR");
  CHECK(body["errors"]["fields"]["email"][0] == "email must be a valid email");
}

TEST_CASE("the unmatched-route answers are the substrate's own")
{
  const auto missing = bodyOf(ErrorHandler::unmatchedRoute(drogon::k404NotFound));
  CHECK(missing["status"] == 404);
  CHECK(missing["errors"]["code"] == "NOT_FOUND");

  const auto wrongMethod =
      bodyOf(ErrorHandler::unmatchedRoute(drogon::k405MethodNotAllowed));
  CHECK(wrongMethod["status"] == 405);
  CHECK(wrongMethod["errors"]["code"] == "METHOD_NOT_ALLOWED");
}

TEST_CASE("ApiResponse::validationError returns 422 with the field errors")
{
  Json::Value emailErrors;
  emailErrors.append("email must be a valid email");
  Json::Value nameErrors;
  nameErrors.append("name must not be empty");
  Json::Value fieldErrors;
  fieldErrors["email"] = emailErrors;
  fieldErrors["name"] = nameErrors;

  const auto response = ApiResponse::validationError(fieldErrors);
  CHECK(response->getStatusCode() == drogon::k422UnprocessableEntity);

  const auto body = bodyOf(response);
  CHECK(body["status"] == 422);
  CHECK(body["info"].isNull());
  CHECK(body["errors"]["code"] == "VALIDATION_ERROR");
  CHECK(body["errors"]["message"] == "Validation failed");
  CHECK(body["errors"]["fields"]["email"][0] == "email must be a valid email");
  CHECK(body["errors"]["fields"]["name"][0] == "name must not be empty");
}

TEST_CASE("Cors::apply writes the four browser headers every service sends")
{
  const auto response = drogon::HttpResponse::newHttpResponse();
  Cors::apply(response);

  CHECK(response->getHeader("Access-Control-Allow-Origin") == "*");
  CHECK(response->getHeader("Access-Control-Allow-Methods") ==
        "GET, POST, PATCH, PUT, DELETE, OPTIONS");
  CHECK(response->getHeader("Access-Control-Allow-Headers") ==
        "Content-Type, Authorization, Accept");
  CHECK(response->getHeader("Access-Control-Max-Age") == "86400");
}

TEST_CASE("Cors::handleOptions answers the pre-routing probe with no body")
{
  drogon::HttpResponsePtr response;
  Cors::handleOptions({}, [&](const auto& result) { response = result; });
  REQUIRE(response);

  CHECK(response->getStatusCode() == drogon::k200OK);
  CHECK(response->getHeader("Access-Control-Allow-Origin") == "*");
  CHECK(response->getBody().empty());
}

TEST_CASE("a field of the wrong type is a 422, not a 500")
{
  Json::Value body(Json::objectValue);
  body["title"] = Json::Value(Json::objectValue);
  try {
    static_cast<void>(body["title"].asString());
    FAIL("jsoncpp accepted an object as a string");
  }
  catch (const std::exception& error) {
    const auto response = answerTo(error);
    CHECK(response->getStatusCode() == drogon::k422UnprocessableEntity);
    CHECK(bodyOf(response)["status"] == 422);
  }
}
