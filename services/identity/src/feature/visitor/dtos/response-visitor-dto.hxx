#pragma once

#include <feature/visitor/repositories/visitor/visitor-query.hxx>
#include <json/value.h>
#include <optional>
#include <vector>

struct ResponseVisitorDto
{
  VisitorRow visitor;

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponseVisitorListDto
{
  std::vector<VisitorRow> visitors;
  bool recognitionEnabled{false};
  std::optional<VisitorCursor> nextCursor;

  [[nodiscard]] Json::Value toJson() const;
};
