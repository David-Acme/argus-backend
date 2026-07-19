#pragma once

#include <stdexcept>
#include <string>

class ResponseException : public std::runtime_error
{
public:
  explicit ResponseException(const std::string& message,
                             int statusCode = 400)
      : std::runtime_error(message), statusCode_(statusCode)
  {
  }

  int statusCode() const { return statusCode_; }

private:
  int statusCode_;
};
