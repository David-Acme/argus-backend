#pragma once

#include <string_view>

namespace proxy_allowlist
{
struct MembershipInput
{
  std::string_view configured;
  std::string_view address;
};

[[nodiscard]] bool contains(const MembershipInput& input);
}
