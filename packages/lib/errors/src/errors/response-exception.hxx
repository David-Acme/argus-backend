#pragma once

#include "error-definition.hxx"

#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

struct ResponseError
{
  std::string code;
  std::string message;
};

using ResponseErrors = std::variant<ResponseError, std::vector<ResponseError>>;

struct ResponseExceptionInput
{
  std::string message;
  int statusCode{400};
  std::string errorCode{"ERROR"};
};

class ResponseException : public std::runtime_error
{
public:
  explicit ResponseException(const ErrorDefinition& error);
  ResponseException(int statusCode, const ErrorDefinition& error);
  ResponseException(int statusCode, std::vector<ResponseError> errors);
  explicit ResponseException(std::string message);
  explicit ResponseException(const ResponseExceptionInput& input);

  int statusCode() const noexcept;
  const std::string& errorCode() const noexcept;
  const ResponseErrors& errors() const noexcept;

private:
  int statusCode_;
  ResponseErrors errors_;
};
