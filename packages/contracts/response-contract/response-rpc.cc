#include "response-rpc.hxx"

#include <response.pb.h>
#include <algorithm>
#include <string_view>
#include <utility>

namespace argus::response
{
namespace
{
constexpr std::size_t kMaxDetails = 4096;
constexpr int kMaxErrors = 16;
constexpr ErrorDefinition kInternal{
    .code = ErrorCode::InternalError, .message = "Internal service error"};
constexpr ErrorDefinition kInvalidResponse{
    .code = ErrorCode::BadGateway, .message = "Invalid service response"};

grpc::StatusCode rpcCode(int status)
{
  switch (status) {
    case 400: case 422: return grpc::StatusCode::INVALID_ARGUMENT;
    case 401: return grpc::StatusCode::UNAUTHENTICATED;
    case 403: return grpc::StatusCode::PERMISSION_DENIED;
    case 404: return grpc::StatusCode::NOT_FOUND;
    case 409: return grpc::StatusCode::ALREADY_EXISTS;
    case 429: return grpc::StatusCode::RESOURCE_EXHAUSTED;
    case 499: return grpc::StatusCode::CANCELLED;
    case 502: case 503: return grpc::StatusCode::UNAVAILABLE;
    case 504: return grpc::StatusCode::DEADLINE_EXCEEDED;
    default: return grpc::StatusCode::INTERNAL;
  }
}

bool validRecord(const v1::ErrorRecord& error)
{
  return !error.code().empty() && error.code().size() <= 128 &&
         std::ranges::all_of(error.code(), [](unsigned char value) {
           return value >= 0x21 && value <= 0x7e;
         }) &&
         !error.message().empty() && error.message().size() <= 1024 &&
         error.message().find('\0') == std::string::npos;
}

bool validResponse(const v1::ErrorResponse& response)
{
  if (response.status() < 400 || response.status() > 599 ||
      response.ByteSizeLong() > kMaxDetails)
    return false;
  if (response.has_single())
    return validRecord(response.single());
  return response.has_list() && response.list().errors_size() > 0 &&
         response.list().errors_size() <= kMaxErrors &&
         std::ranges::all_of(response.list().errors(), validRecord);
}

void assignRecord(v1::ErrorRecord& target, const ResponseError& source)
{
  target.set_code(source.code);
  target.set_message(source.message);
}

ResponseException transportError(grpc::StatusCode code)
{
  switch (code) {
    case grpc::StatusCode::CANCELLED:
      return ResponseException(499, {.code = ErrorCode::Cancelled, .message = "Request cancelled"});
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return ResponseException(504, {.code = ErrorCode::DeadlineExceeded, .message = "Request deadline exceeded"});
    case grpc::StatusCode::UNAUTHENTICATED:
      return ResponseException(401, {.code = ErrorCode::Unauthorized, .message = "Service credential required"});
    case grpc::StatusCode::PERMISSION_DENIED:
      return ResponseException(403, {.code = ErrorCode::Forbidden, .message = "Service access denied"});
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::OUT_OF_RANGE:
    case grpc::StatusCode::FAILED_PRECONDITION:
      return ResponseException(400, {.code = ErrorCode::BadRequest, .message = "Invalid service request"});
    case grpc::StatusCode::NOT_FOUND:
      return ResponseException(404, {.code = ErrorCode::NotFound, .message = "Service resource not found"});
    case grpc::StatusCode::ALREADY_EXISTS:
    case grpc::StatusCode::ABORTED:
      return ResponseException(409, {.code = ErrorCode::Conflict, .message = "Service request conflict"});
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return ResponseException(429, {.code = ErrorCode::TooManyRequests, .message = "Service busy"});
    case grpc::StatusCode::UNAVAILABLE:
      return ResponseException(503, {.code = ErrorCode::ServiceUnavailable, .message = "Service unavailable"});
    default:
      return ResponseException(500, kInternal);
  }
}
}

grpc::Status toRpcStatus(const ResponseException& error)
{
  v1::ErrorResponse response;
  response.set_status(error.statusCode());
  if (const auto* single = std::get_if<ResponseError>(&error.errors())) {
    assignRecord(*response.mutable_single(), *single);
  } else {
    const auto& errors = std::get<std::vector<ResponseError>>(error.errors());
    if (errors.size() > static_cast<std::size_t>(kMaxErrors))
      return toRpcStatus(ResponseException(500, kInternal));
    for (const auto& record : errors)
      assignRecord(*response.mutable_list()->add_errors(), record);
  }
  if (!validResponse(response))
    return toRpcStatus(ResponseException(500, kInternal));
  return {rpcCode(error.statusCode()), "Service request failed", response.SerializeAsString()};
}

ResponseException fromRpcStatus(const grpc::Status& status)
{
  const auto& details = status.error_details();
  if (details.empty())
    return transportError(status.error_code());
  v1::ErrorResponse response;
  if (details.size() > kMaxDetails || !response.ParseFromString(details) ||
      !validResponse(response) || rpcCode(static_cast<int>(response.status())) != status.error_code())
    return ResponseException(502, kInvalidResponse);
  if (response.has_single())
    return ResponseException({.message = response.single().message(),
                              .statusCode = static_cast<int>(response.status()),
                              .errorCode = response.single().code()});
  std::vector<ResponseError> errors;
  errors.reserve(response.list().errors_size());
  for (const auto& record : response.list().errors())
    errors.push_back({.code = record.code(), .message = record.message()});
  return ResponseException(static_cast<int>(response.status()), std::move(errors));
}
}
