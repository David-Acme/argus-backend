#include "create-project-member-dto.hxx"

CreateProjectMemberDto CreateProjectMemberDto::fromJson(const Json::Value& json)
{
  CreateProjectMemberDto dto;
  if (json.isMember("projectId") && json["projectId"].isInt64())
    dto.projectId = json["projectId"].asInt64();
  if (json.isMember("userId") && json["userId"].isInt64())
    dto.userId = json["userId"].asInt64();
  dto.access = json.get("access", "view").asString();

  START_VALIDATION(CreateProjectMemberDto, dto)
  IS_POSITIVE(projectId)
  IS_POSITIVE(userId)
  IS_IN(access, "view", "edit")
  END_VALIDATION()
  return dto;
}
