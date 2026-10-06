#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace text_norm
{

enum class NameMatchKind : std::uint8_t
{
  Exact,
  Missing,
  Ambiguous
};

struct NameMatch
{
  NameMatchKind kind{NameMatchKind::Missing};
  std::vector<std::size_t> hits;
};

[[nodiscard]] std::string folded(const std::string& text);

[[nodiscard]] NameMatch matchName(const std::vector<std::string>& names, const std::string& asked);

}
