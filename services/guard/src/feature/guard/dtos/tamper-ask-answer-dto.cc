#include "tamper-ask-answer-dto.hxx"

#include <validation/validation_dsl.hxx>

TamperAskAnswerDto TamperAskAnswerDto::fromJson(const Json::Value& json)
{
  TamperAskAnswerDto dto;
  dto.answer = json.get("answer", "").asString();

  START_VALIDATION(TamperAskAnswerDto, dto)
  IS_IN(answer, "me", "no")
  END_VALIDATION()
  return dto;
}
