#pragma once

#include "error-code.hxx"

#include <string_view>

struct ErrorDefinition
{
  ErrorCode code;
  std::string_view message;
};
