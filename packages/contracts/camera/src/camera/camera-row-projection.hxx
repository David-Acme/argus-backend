#pragma once

#include <json/value.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace camera_projection
{
inline constexpr std::array<std::string_view, 5> kConnectionFields = {
    "ip", "port", "username", "cloudUsername", "config"};

inline bool isConnectionField(std::string_view key)
{
  return std::ranges::any_of(kConnectionFields, [key](std::string_view field) {
    return key == field ||
           (key.size() > field.size() && key.starts_with(field) &&
            key[field.size()] == '.');
  });
}

inline void reduceRow(Json::Value& row)
{
  if (!row.isObject())
    return;
  row["ip"] = "";
  row["port"] = 0;
  row["username"] = "";
  row["cloudUsername"] = "";
  row["config"] = "{}";
}

inline void reduceDiff(Json::Value& changes)
{
  if (!changes.isObject())
    return;
  for (const std::string& key : changes.getMemberNames()) {
    if (isConnectionField(key))
      changes.removeMember(key);
  }
}
}
