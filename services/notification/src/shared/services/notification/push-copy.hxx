#pragma once

#include <json/value.h>

#include <cstdint>
#include <string>

struct PushCopy
{
  std::string title;
  std::string body;
};

struct PushCopyInput
{
  std::string lang;
  std::string urgency;
  bool call{false};
};

namespace push_copy
{
[[nodiscard]] PushCopy render(const PushCopyInput& input);

[[nodiscard]] std::string langOf(const Json::Value& data);

[[nodiscard]] std::string urgencyOf(const Json::Value& data);
}
