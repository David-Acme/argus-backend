#include "rtc-decline-dto.hxx"

#include <feature/rtc/services/rtc-naming.hxx>
#include <validation/validation_dsl.hxx>

RtcDeclineDto RtcDeclineDto::fromJson(const Json::Value& json)
{
  RtcDeclineDto dto;
  if (json.isObject()) {
    if (json["callId"].isString())
      dto.callId = json["callId"].asString();
    else if (json.isMember("callId") && !json["callId"].isNull())
      dto.callId = "?";
  }

  START_VALIDATION(RtcDeclineDto, dto)
  IS_NOT_EMPTY(callId)
  MAX_LENGTH(callId, 64)
  CUSTOM_LAMBDA(callId, [](const RtcDeclineDto& value) -> std::optional<std::string> {
    if (rtc_naming::callKindOf(value.callId) != rtc_naming::CallKind::Invalid)
      return std::nullopt;
    return "callId must be rtc-<32 hex> or call-<id>";
  })
  END_VALIDATION()
  return dto;
}
