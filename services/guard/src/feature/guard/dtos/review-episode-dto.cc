#include "review-episode-dto.hxx"

#include <validation/validation_dsl.hxx>

ReviewEpisodeDto ReviewEpisodeDto::fromJson(const Json::Value& json)
{
  ReviewEpisodeDto dto;
  dto.label = json.get("label", "").asString();

  START_VALIDATION(ReviewEpisodeDto, dto)
  IS_NOT_EMPTY(label)
  IS_IN(label, "useful", "false_alarm", "not_now")
  END_VALIDATION()
  return dto;
}
