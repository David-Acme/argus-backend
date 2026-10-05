#pragma once

#include <feature/visitor/repositories/visitor/visitor-query.hxx>
#include <feature/visitor/services/visit-pattern.hxx>
#include <json/value.h>
#include <vector>

struct ResponseVisitorDetailDto
{
  VisitorRow visitor;
  std::vector<VisitorSampleRow> samples;
  std::vector<VisitorVisitRow> visits;
  VisitPatternSummary pattern;

  [[nodiscard]] Json::Value toJson() const;
};
