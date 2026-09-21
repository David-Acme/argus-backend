#pragma once

#include <cstdint>
#include <string>

// What a member may do with a record shared with them; `View` is the default.
enum class ShareAccess : uint8_t
{
  View = 0,
  Edit
};

inline std::string shareAccessToString(ShareAccess a)
{
  return a == ShareAccess::Edit ? "edit" : "view";
}

inline ShareAccess shareAccessFromString(const std::string& s)
{
  return s == "edit" ? ShareAccess::Edit : ShareAccess::View;
}
