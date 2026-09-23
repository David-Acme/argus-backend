#include "response-exception.hxx"

#include "error-definition.hxx"

#include <string>
#include <utility>
#include <vector>

namespace
{
std::string firstMessage(const std::vector<ResponseError>& errors)
{
  if (errors.empty())
    throw std::invalid_argument("Response errors must not be empty");
  return errors.front().message;
}
}

ResponseException::ResponseException(const ErrorDefinition& error)
    : ResponseException(ResponseExceptionInput{
          .message = std::string(error.message),
          .statusCode = error.status,
          .errorCode = std::string(toString(error.code))})
{
}

ResponseException::ResponseException(int statusCode, const ErrorDefinition& error)
    : ResponseException(ResponseExceptionInput{
          .message = std::string(error.message),
          .statusCode = statusCode,
          .errorCode = std::string(toString(error.code))})
{
}

ResponseException::ResponseException(int statusCode,
                                     std::vector<ResponseError> errors)
    : std::runtime_error(firstMessage(errors)), statusCode_(statusCode),
      errors_(std::move(errors))
{
}

ResponseException::ResponseException(std::string message)
    : ResponseException(ResponseExceptionInput{.message = std::move(message),
                                                .statusCode = 400,
                                                .errorCode = "ERROR"})
{
}

ResponseException::ResponseException(const ResponseExceptionInput& input)
    : std::runtime_error(input.message), statusCode_(input.statusCode),
      errors_(ResponseError{.code = input.errorCode, .message = input.message})
{
}

int ResponseException::statusCode() const noexcept
{
  return statusCode_;
}

const std::string& ResponseException::errorCode() const noexcept
{
  if (const auto* error = std::get_if<ResponseError>(&errors_))
    return error->code;
  return std::get<std::vector<ResponseError>>(errors_).front().code;
}

const ResponseErrors& ResponseException::errors() const noexcept
{
  return errors_;
}
