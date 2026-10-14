#pragma once

#include <json/value.h>

struct ResponseRtcDeclineDto
{
  bool declined{true};

  [[nodiscard]] Json::Value toJson() const;
};
