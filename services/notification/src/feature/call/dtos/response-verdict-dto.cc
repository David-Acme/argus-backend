#include "response-verdict-dto.hxx"

#include <validation/validation_dsl.hxx>

ResponseVerdictDto ResponseVerdictDto::fromJson(const Json::Value& json)
{
  ResponseVerdictDto dto;
  dto.verdict = json["verdict"].isString() ? json["verdict"].asString() : std::string{};

  START_VALIDATION(ResponseVerdictDto, dto)
  IS_NOT_EMPTY(verdict)
  IS_IN(verdict, "real", "false_alarm")
  END_VALIDATION()
  return dto;
}
