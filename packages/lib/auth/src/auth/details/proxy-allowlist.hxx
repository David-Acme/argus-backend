#pragma once

#include <string>
#include <string_view>

namespace proxy_allowlist
{
struct MembershipInput
{
  std::string_view configured;
  std::string_view address;
};

[[nodiscard]] bool contains(const MembershipInput& input);

struct PrefixInput
{
  std::string_view address;
  int ipv4Bits{32};
  int ipv6Bits{128};
};

[[nodiscard]] std::string prefixOf(const PrefixInput& input);
}
