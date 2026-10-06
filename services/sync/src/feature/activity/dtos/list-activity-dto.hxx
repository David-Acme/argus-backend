#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>
#include <optional>
#include <string>

struct ListActivityDto
{
  static constexpr int kDefaultLimit = 50;
  static constexpr int kMaxLimit = 200;

  std::optional<std::string> module;
  std::optional<std::string> action;
  std::optional<std::string> table;
  std::optional<int64_t> userId;
  std::optional<int64_t> from;
  std::optional<int64_t> to;
  int limit{kDefaultLimit};
  std::optional<std::string> cursor;

  static ListActivityDto fromRequest(const drogon::HttpRequestPtr& request);
};
