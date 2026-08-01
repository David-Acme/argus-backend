#pragma once

#include <stdexcept>
#include <string>

class ResponseException : public std::runtime_error
{
public:
  explicit ResponseException(const std::string& message, int statusCode = 400,
                             const std::string& errorCode = "ERROR")
      : std::runtime_error(message), statusCode_(statusCode),
        errorCode_(errorCode)
  {
  }

  int statusCode() const { return statusCode_; }
  const std::string& errorCode() const { return errorCode_; }

private:
  int statusCode_;
  std::string errorCode_;
};
