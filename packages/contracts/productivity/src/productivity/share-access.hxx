#pragma once

#include <cstdint>
#include <string>

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
