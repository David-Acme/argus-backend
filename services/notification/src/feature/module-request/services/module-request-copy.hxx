#pragma once

#include <string>
#include <string_view>

struct ModuleRequestText
{
  std::string title;
  std::string body;
};

struct ModuleRequestCopyInput
{
  std::string_view lang;
  std::string_view requester;
  std::string_view module;
};

namespace module_request_copy
{
[[nodiscard]] ModuleRequestText render(const ModuleRequestCopyInput& input);
}
