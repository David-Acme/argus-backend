#include "update-project-member-dto.hxx"

UpdateProjectMemberDto UpdateProjectMemberDto::fromJson(const Json::Value& json)
{
  UpdateProjectMemberDto dto;
  dto.access = json.get("access", "").asString();

  START_VALIDATION(UpdateProjectMemberDto, dto)
  IS_IN(access, "view", "edit")
  END_VALIDATION()
  return dto;
}
