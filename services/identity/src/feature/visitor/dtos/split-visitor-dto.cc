#include "split-visitor-dto.hxx"

#include <feature/visitor/dtos/id-list.hxx>
#include <validation/validation_dsl.hxx>

SplitVisitorDto SplitVisitorDto::fromJson(const Json::Value& json)
{
  SplitVisitorDto dto;
  dto.sampleIds = visitor_dto::idsOf(json["sampleIds"]);
  START_VALIDATION(SplitVisitorDto, dto)
  ARRAY_NOT_EMPTY(sampleIds, int64_t)
  MAX_ELEMENTS(sampleIds, int64_t, 64)
  END_VALIDATION()
  return dto;
}
