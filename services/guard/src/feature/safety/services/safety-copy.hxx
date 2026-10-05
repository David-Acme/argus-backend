#pragma once

#include <feature/safety/vocabulary/safety-alert-kind.hxx>

#include <string>
#include <string_view>

struct SafetyText
{
  std::string title;
  std::string body;
};

namespace safety_copy
{
[[nodiscard]] SafetyText panicSent(std::string_view lang);
}
