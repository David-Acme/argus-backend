#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

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
