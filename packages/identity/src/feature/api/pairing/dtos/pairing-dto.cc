#include "pairing-dto.hxx"

PairingDto PairingDto::fromJson(const Json::Value& json)
{
  PairingDto dto;
  dto.code = json.get("code", "").asString();

  START_VALIDATION(PairingDto, dto)
  IS_NOT_EMPTY(code)
  IS_ALNUM(code)
  MIN_LENGTH(code, 6)
  MAX_LENGTH(code, 12)
  END_VALIDATION()

  return dto;
}
