#pragma once

#include <json/value.h>
#include <string>
#include <vector>

struct ResponseRevokeSessionsDto
{
  std::vector<std::string> revoked;
  bool current{false};

  [[nodiscard]] Json::Value toJson() const;
};
