#pragma once

#include <string>
#include <string_view>

namespace sql_util
{
// Doubles embedded single quotes so the value is safe inside a SQL literal.
inline std::string escapeLiteral(std::string_view value)
{
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    out.push_back(c);
    if (c == '\'')
      out.push_back('\'');
  }
  return out;
}
} // namespace sql_util
