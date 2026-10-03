#pragma once

#include <json/value.h>
#include <optional>
#include <string>

struct SocketFrameDto
{
  std::string type;

  static std::optional<SocketFrameDto> fromJson(const Json::Value& frame)
  {
    if (!frame.isObject())
      return std::nullopt;
    const Json::Value& type = frame["type"];
    return SocketFrameDto{.type = type.isString() ? type.asString() : std::string{}};
  }
};
