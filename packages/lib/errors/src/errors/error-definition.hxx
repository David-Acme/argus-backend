#pragma once

#include "error-code.hxx"

#include <string_view>

struct ErrorDefinition
{
  ErrorCode code;
  int status;
  std::string_view message;

  [[nodiscard]] constexpr ErrorDefinition
  withMessage(std::string_view detail) const
  {
    return ErrorDefinition{code, status, detail};
  }

  [[nodiscard]] constexpr std::string_view wireCode() const
  {
    return toString(code);
  }
};
