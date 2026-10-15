#pragma once

#include <json/value.h>

#include <cstdint>
#include <string>

namespace push_alert_intent
{

struct Input
{
  std::string intentId;
  int64_t notificationId{0};
  std::string type;
  std::string title;
  std::string body;
  std::string urgency;
  std::string episode;
  bool ringFollows{false};
  int64_t issuedAt{0};
  int64_t expiresAt{0};
};

[[nodiscard]] Json::Value build(const Input& input);

}
