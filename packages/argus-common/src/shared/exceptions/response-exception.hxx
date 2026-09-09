#pragma once

#include <stdexcept>
#include <string>
#include <utility>

struct ResponseExceptionInput
{
  std::string message;
  int statusCode{400};
  std::string errorCode{"ERROR"};
};

class ResponseException : public std::runtime_error
{
public:
  explicit ResponseException(std::string message)
      : ResponseException(ResponseExceptionInput{.message = std::move(message)})
  {
  }

  explicit ResponseException(const ResponseExceptionInput& input)
      : std::runtime_error(input.message), statusCode_(input.statusCode),
        errorCode_(input.errorCode)
  {
  }

  int statusCode() const { return statusCode_; }
  const std::string& errorCode() const { return errorCode_; }

private:
  int statusCode_;
  std::string errorCode_;
};
