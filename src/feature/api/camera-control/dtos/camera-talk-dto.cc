#include "camera-talk-dto.hxx"

CameraTalkDto CameraTalkDto::fromJson(const Json::Value& json)
{
  CameraTalkDto dto;
  dto.text = json.get("text", "").asString();
  dto.lang = json.get("lang", "es").asString();

  START_VALIDATION(CameraTalkDto, dto)
  IS_NOT_EMPTY(text)
  MAX_LENGTH(text, 300)
  IS_IN(lang, "es", "en")
  END_VALIDATION()
  return dto;
}
