#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace relay_recipient
{

struct Member
{
  int64_t userId{0};
  std::string name;
};

enum class ResolutionKind : std::uint8_t
{
  Resolved,
  Ambiguous,
  Missing,
  Self
};

struct Resolution
{
  ResolutionKind kind{ResolutionKind::Missing};
  int64_t userId{0};
  std::vector<int64_t> candidates;
};

struct ResolveInput
{
  const std::vector<Member>& roster;
  std::string_view asked;
  int64_t senderId{0};
};

[[nodiscard]] Resolution resolve(const ResolveInput& input);

}
