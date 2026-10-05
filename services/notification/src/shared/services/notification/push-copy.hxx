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

struct PushDataInput
{
  int64_t notificationId{0};
  const Json::Value& data;
};

namespace push_copy
{
[[nodiscard]] PushCopy render(const PushCopyInput& input);

[[nodiscard]] Json::Value minimalData(const PushDataInput& input);

[[nodiscard]] std::string langOf(const Json::Value& data);

[[nodiscard]] std::string urgencyOf(const Json::Value& data);
}
