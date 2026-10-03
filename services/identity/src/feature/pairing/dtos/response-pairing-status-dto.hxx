#pragma once

#include <json/value.h>

struct ResponsePairingStatusDto
{
  bool paired{false};
  bool hasOwner{false};

  [[nodiscard]] Json::Value toJson() const;
};
