#include "list-visitors-dto.hxx"

#include <validation/validation_dsl.hxx>

ListVisitorsDto ListVisitorsDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  ListVisitorsDto dto;
  dto.scope = request->getParameter("scope");
  if (dto.scope.empty())
    dto.scope = "all";
  START_VALIDATION(ListVisitorsDto, dto)
  IS_IN(scope, "all", "named")
  END_VALIDATION()
  return dto;
}
