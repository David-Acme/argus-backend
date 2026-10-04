#include "revoke-sessions-dto.hxx"

RevokeSessionsDto RevokeSessionsDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  RevokeSessionsDto dto;
  dto.scope = request->getParameter("scope");

  START_VALIDATION(RevokeSessionsDto, dto)
  IS_NOT_EMPTY(scope)
  IS_IN(scope, "others", "all")
  END_VALIDATION()

  dto.target = dto.scope == "all" ? SessionRevocationScope::All
                                  : SessionRevocationScope::Others;
  return dto;
}
