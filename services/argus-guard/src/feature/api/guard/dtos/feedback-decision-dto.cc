#include "feedback-decision-dto.hxx"

#include <shared/validation/validation_dsl.hxx>

FeedbackDecisionDto FeedbackDecisionDto::fromJson(const Json::Value& json)
{
  FeedbackDecisionDto dto;
  dto.label = json.get("label", "").asString();

  START_VALIDATION(FeedbackDecisionDto, dto)
  IS_NOT_EMPTY(label)
  IS_IN(label, "useful", "false_alarm", "not_now")
  END_VALIDATION()
  return dto;
}
