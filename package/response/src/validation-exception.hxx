#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using ValidationErrors = std::map<std::string, std::vector<std::string>>;

class ValidationException : public std::runtime_error
{
public:
  explicit ValidationException(const ValidationErrors& errors,
                               int statusCode = 422)
      : std::runtime_error("Validation failed"), errors_(errors),
        statusCode_(statusCode)
  {
  }

  const ValidationErrors& errors() const noexcept { return errors_; }
  int statusCode() const noexcept { return statusCode_; }

private:
  ValidationErrors errors_;
  int statusCode_;
};
