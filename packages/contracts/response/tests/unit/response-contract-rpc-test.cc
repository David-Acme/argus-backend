#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>
#include <errors/response-exception.hxx>
#include <grpcpp/support/status.h>
#include <response/response-rpc.hxx>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using argus::response::fromRpcStatus;
using argus::response::toRpcStatus;

namespace
{
constexpr ErrorDefinition kNotFound{.code = ErrorCode::NotFound,
                                    .status = 404,
                                    .message = "Widget not found"};
constexpr ErrorDefinition kBadRequest{.code = ErrorCode::BadRequest,
                                      .status = 400,
                                      .message = "Nope"};

grpc::Status transportStatus(grpc::StatusCode code)
{
  return {code, "transport said no", std::string{}};
}

constexpr std::size_t kMaxDetailsBytes = 4096;
}

TEST_CASE("a single refusal round-trips through the rpc status")
{
  const ResponseException refused(404, kNotFound);
  const grpc::Status status = toRpcStatus(refused);
  CHECK(status.error_code() == grpc::StatusCode::NOT_FOUND);
  CHECK(!status.error_details().empty());

  const ResponseException back = fromRpcStatus(status);
  CHECK(back.statusCode() == 404);
  CHECK(back.errorCode() == "NOT_FOUND");
  CHECK(std::string(back.what()) == "Widget not found");
  CHECK(std::get_if<ResponseError>(&back.errors()) != nullptr);
}

TEST_CASE("a list refusal keeps its order and answers with its first record")
{
  const ResponseException
      refused{422, std::vector<ResponseError>{{"VALIDATION_ERROR", "field a"},
                                              {"BAD_REQUEST", "field b"}}};
  const ResponseException back = fromRpcStatus(toRpcStatus(refused));
  CHECK(back.statusCode() == 422);
  CHECK(back.errorCode() == "VALIDATION_ERROR");
  CHECK(std::string(back.what()) == "field a");

  const auto* errors = std::get_if<std::vector<ResponseError>>(&back.errors());
  REQUIRE(errors != nullptr);
  REQUIRE(errors->size() == 2);
  CHECK((*errors)[0].code == "VALIDATION_ERROR");
  CHECK((*errors)[0].message == "field a");
  CHECK((*errors)[1].code == "BAD_REQUEST");
  CHECK((*errors)[1].message == "field b");
}

TEST_CASE("the rpc code follows the status on the way out")
{
  const std::vector<std::pair<int, grpc::StatusCode>>
      cases{{400, grpc::StatusCode::INVALID_ARGUMENT},
            {401, grpc::StatusCode::UNAUTHENTICATED},
            {403, grpc::StatusCode::PERMISSION_DENIED},
            {404, grpc::StatusCode::NOT_FOUND},
            {409, grpc::StatusCode::ALREADY_EXISTS},
            {418, grpc::StatusCode::INTERNAL},
            {422, grpc::StatusCode::INVALID_ARGUMENT},
            {429, grpc::StatusCode::RESOURCE_EXHAUSTED},
            {499, grpc::StatusCode::CANCELLED},
            {500, grpc::StatusCode::INTERNAL},
            {502, grpc::StatusCode::UNAVAILABLE},
            {503, grpc::StatusCode::UNAVAILABLE},
            {504, grpc::StatusCode::DEADLINE_EXCEEDED}};
  for (const auto& [code, rpc] : cases) {
    CAPTURE(code);
    const ResponseException refused{
        ResponseExceptionInput{.message = "refused",
                               .statusCode = code,
                               .errorCode = "ERROR"}};
    CHECK(toRpcStatus(refused).error_code() == rpc);
  }
}

TEST_CASE("a transport failure with no payload becomes the documented refusal")
{
  const std::vector<std::pair<grpc::StatusCode, std::pair<int, std::string>>>
      cases{{grpc::StatusCode::CANCELLED, {499, "CANCELLED"}},
            {grpc::StatusCode::UNKNOWN, {500, "INTERNAL_ERROR"}},
            {grpc::StatusCode::INVALID_ARGUMENT, {400, "BAD_REQUEST"}},
            {grpc::StatusCode::DEADLINE_EXCEEDED, {504, "DEADLINE_EXCEEDED"}},
            {grpc::StatusCode::NOT_FOUND, {404, "NOT_FOUND"}},
            {grpc::StatusCode::ALREADY_EXISTS, {409, "CONFLICT"}},
            {grpc::StatusCode::PERMISSION_DENIED, {403, "FORBIDDEN"}},
            {grpc::StatusCode::RESOURCE_EXHAUSTED, {429, "TOO_MANY_REQUESTS"}},
            {grpc::StatusCode::FAILED_PRECONDITION, {400, "BAD_REQUEST"}},
            {grpc::StatusCode::ABORTED, {409, "CONFLICT"}},
            {grpc::StatusCode::OUT_OF_RANGE, {400, "BAD_REQUEST"}},
            {grpc::StatusCode::UNAUTHENTICATED, {401, "UNAUTHORIZED"}},
            {grpc::StatusCode::INTERNAL, {500, "INTERNAL_ERROR"}},
            {grpc::StatusCode::UNAVAILABLE, {503, "SERVICE_UNAVAILABLE"}}};
  for (const auto& [rpc, expected] : cases) {
    const auto& [code, wire] = expected;
    CAPTURE(code);
    const ResponseException back = fromRpcStatus(transportStatus(rpc));
    CHECK(back.statusCode() == code);
    CHECK(back.errorCode() == wire);
    CHECK(back.errors().index() == 0);
  }

  const ResponseException ok = fromRpcStatus(grpc::Status{});
  CHECK(ok.statusCode() == 500);
  CHECK(ok.errorCode() == "INTERNAL_ERROR");
}

TEST_CASE("a refusal the wire cannot carry is answered as an internal error")
{
  const std::string tooLong(1025, 'x');
  const std::vector<ResponseException> uncarriable{
      ResponseException(400, kBadRequest.withMessage(tooLong)),
      ResponseException(ResponseExceptionInput{.message = "not an error",
                                               .statusCode = 200,
                                               .errorCode = "ERROR"}),
      ResponseException(400, std::vector<ResponseError>(17, {"BAD_REQUEST",
                                                             "nope"}))};
  for (const auto& refused : uncarriable) {
    const grpc::Status status = toRpcStatus(refused);
    CHECK(status.error_code() == grpc::StatusCode::INTERNAL);
    const ResponseException back = fromRpcStatus(status);
    CHECK(back.statusCode() == 500);
    CHECK(back.errorCode() == "INTERNAL_ERROR");
    CHECK(std::string(back.what()) == "Internal service error");
  }
}

TEST_CASE("a payload that does not match its rpc code is refused, not trusted")
{
  const grpc::Status honest = toRpcStatus(ResponseException(404, kNotFound));
  const ResponseException mismatched{fromRpcStatus(
      grpc::Status{grpc::StatusCode::INTERNAL, honest.error_message(),
                   honest.error_details()})};
  CHECK(mismatched.statusCode() == 502);
  CHECK(mismatched.errorCode() == "BAD_GATEWAY");
  CHECK(std::string(mismatched.what()) == "Invalid service response");

  const ResponseException truncated{
      fromRpcStatus({grpc::StatusCode::UNKNOWN, "transport said no",
                     std::string("\x0a\xff", 2)})};
  CHECK(truncated.statusCode() == 502);
  CHECK(truncated.errorCode() == "BAD_GATEWAY");

  const ResponseException oversized{
      fromRpcStatus({grpc::StatusCode::UNKNOWN, "transport said no",
                     std::string(kMaxDetailsBytes + 1, 'x')})};
  CHECK(oversized.statusCode() == 502);
  CHECK(oversized.errorCode() == "BAD_GATEWAY");
}
