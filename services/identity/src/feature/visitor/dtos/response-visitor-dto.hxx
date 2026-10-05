#pragma once

#include <feature/visitor/repositories/visitor/visitor-query.hxx>
#include <json/value.h>
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

  [[nodiscard]] Json::Value toJson() const;
};
