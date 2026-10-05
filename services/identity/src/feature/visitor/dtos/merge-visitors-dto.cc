#include "merge-visitors-dto.hxx"

#include <feature/visitor/dtos/id-list.hxx>
#include <validation/validation_dsl.hxx>

MergeVisitorsDto MergeVisitorsDto::fromJson(const Json::Value& json)
{
  MergeVisitorsDto dto;
  dto.sourceIds = visitor_dto::idsOf(json["sourceIds"]);
  START_VALIDATION(MergeVisitorsDto, dto)
  ARRAY_NOT_EMPTY(sourceIds, int64_t)
  MAX_ELEMENTS(sourceIds, int64_t, 20)
  END_VALIDATION()
  return dto;
}
