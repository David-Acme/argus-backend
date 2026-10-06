#pragma once

#include "error-definition.hxx"
#include "response-exception.hxx"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace error_list
{
inline constexpr std::string_view kModuleId = "MODULE_ID";
inline constexpr std::string_view kRoleHolder = "ROLE_HOLDER";

[[nodiscard]] inline std::vector<ResponseError> headed(const ErrorDefinition& head,
                                                       std::vector<ResponseError> entries)
{
  entries.insert(entries.begin(),
                 ResponseError{.code = std::string(head.wireCode()), .message = std::string(head.message)});
  return entries;
}

[[nodiscard]] inline ResponseError entry(std::string_view code, std::string message)
{
  return ResponseError{.code = std::string(code), .message = std::move(message)};
}

[[nodiscard]] inline std::vector<ResponseError> forModule(const ErrorDefinition& head, std::string_view moduleId)
{
  std::vector<ResponseError> entries;
  if (!moduleId.empty())
    entries.push_back(entry(kModuleId, std::string(moduleId)));
  return headed(head, std::move(entries));
}
}
