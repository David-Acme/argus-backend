#pragma once

#include <json/value.h>
#include <llm/llm-service.hxx>

#include <cstdint>
#include <optional>
#include <string>

struct ScreenDto
{
  std::string text;
  std::optional<int64_t> userId;
  std::optional<std::string> role;
  std::optional<std::string> surface;

  static ScreenDto fromJson(const Json::Value& json);

  [[nodiscard]] ChatRequest request() const;
};
