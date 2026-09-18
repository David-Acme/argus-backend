#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/app-config.hxx>
#include <response-exception.hxx>
#include <tts-errors.hxx>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <json/json.h>
#include <shared/wrapper/api-response/api-response.hxx>
#include <sstream>
#include <string>

static Json::Value bodyOf(const drogon::HttpResponsePtr& response)
{
    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::istringstream stream{std::string(response->getBody())};
    std::string error;
    CHECK(Json::parseFromStream(builder, stream, &parsed, &error));
    return parsed;
}

static Json::Value sampleData()
{
    Json::Value data;
    data["id"] = 7;
    data["name"] = "Alice";
    return data;
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

TEST_CASE("ApiResponse::error serializes single and array ResponseException errors")
{
    const auto single = bodyOf(ApiResponse::error(ResponseException(
        503, TtsErrors::TtsNotLoaded)));
    CHECK(single["status"] == 503);
    CHECK(single["errors"]["code"] == "TTS_NOT_LOADED");
    CHECK(single["errors"]["message"] == "Text-to-speech engine is not loaded");

    const auto multiple = bodyOf(ApiResponse::error(ResponseException(
        422, std::vector<ResponseError>{
                 {.code = "UNSUPPORTED_LANGUAGE", .message = "Unsupported language"},
                 {.code = "VOICE_UNKNOWN", .message = "Unknown voice"}})));
    CHECK(multiple["status"] == 422);
    CHECK(multiple["errors"].isArray());
    CHECK(multiple["errors"][0]["code"] == "UNSUPPORTED_LANGUAGE");
    CHECK(multiple["errors"][1]["message"] == "Unknown voice");
}

TEST_CASE("ResponseException owns definition messages and preserves legacy codes")
{
    std::string message = "Temporary message";
    const ErrorDefinition definition{.code = ErrorCode::BadRequest,
                                     .message = message};
    const ResponseException error(400, definition);
    message.assign("Changed message");
    const auto copy = error;
    CHECK(std::string(copy.what()) == "Temporary message");
    CHECK(copy.errorCode() == "BAD_REQUEST");
    CHECK(copy.statusCode() == 400);
    CHECK(std::get<ResponseError>(copy.errors()).message == "Temporary message");

    const ResponseException legacy({.message = "Domain failure",
                                    .statusCode = 409,
                                    .errorCode = "EXISTING_DOMAIN_CODE"});
    CHECK(legacy.errorCode() == "EXISTING_DOMAIN_CODE");
    CHECK(legacy.statusCode() == 409);
    const ResponseException plain(std::string("Default failure"));
    CHECK(plain.errorCode() == "ERROR");
    CHECK(plain.statusCode() == 400);
}

TEST_CASE("ResponseException owns error lists and rejects empty lists")
{
    std::vector<ResponseError> errors{
        {.code = "FIRST", .message = "First failure"},
        {.code = "SECOND", .message = "Second failure"}};
    const ResponseException error(422, errors);
    errors.clear();
    const auto copy = error;
    CHECK(copy.errorCode() == "FIRST");
    CHECK(std::string(copy.what()) == "First failure");
    CHECK(std::get<std::vector<ResponseError>>(copy.errors()).size() == 2);
    CHECK_THROWS_AS((ResponseException(422, std::vector<ResponseError>{})),
                    std::invalid_argument);
}

TEST_CASE("AppConfig preserves ResponseException arrays at the HTTP boundary")
{
    const ResponseException error(422, std::vector<ResponseError>{
        {.code = "FIRST", .message = "First failure"},
        {.code = "SECOND", .message = "Second failure"}});
    drogon::HttpResponsePtr response;
    AppConfig::handleException(error, {}, [&](const auto& result) {
        response = result;
    });
    REQUIRE(response);
    CHECK(response->getStatusCode() == drogon::k422UnprocessableEntity);
    const auto body = bodyOf(response);
    CHECK(body["errors"].isArray());
    CHECK(body["errors"].size() == 2);
    CHECK(body["info"].isNull());
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
