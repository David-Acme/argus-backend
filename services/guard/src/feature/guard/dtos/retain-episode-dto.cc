#include "retain-episode-dto.hxx"

#include <validation/validation_dsl.hxx>

RetainEpisodeDto RetainEpisodeDto::fromJson(const Json::Value& json)
{
  RetainEpisodeDto dto;
  const bool present = json.isMember("retain") && json["retain"].isBool();
  if (present)
    dto.retain = json["retain"].asBool();
  START_VALIDATION(RetainEpisodeDto, dto)
  CUSTOM_LAMBDA(retain, [present](const RetainEpisodeDto&) -> std::optional<std::string> {
    if (!present)
      return "retain must be true or false";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
