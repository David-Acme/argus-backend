#include "rtc-token-dto.hxx"

#include <feature/rtc/services/rtc-naming.hxx>
#include <validation/validation_dsl.hxx>

RtcTokenDto RtcTokenDto::fromJson(const Json::Value& json)
{
  RtcTokenDto dto;
  if (json.isObject()) {
    if (json["callId"].isString())
      dto.callId = json["callId"].asString();
    else if (json.isMember("callId") && !json["callId"].isNull())
      dto.callId = "?";
    if (json.isMember("resume")) {
      dto.resumeIsBoolean = json["resume"].isBool();
      dto.resume = dto.resumeIsBoolean && json["resume"].asBool();
    }
    if (json["mode"].isString())
      dto.mode = json["mode"].asString();
    else if (json.isMember("mode") && !json["mode"].isNull())
      dto.mode = "?";
  }

  START_VALIDATION(RtcTokenDto, dto)
  MAX_LENGTH(callId, 64)
  CUSTOM_LAMBDA(callId, [](const RtcTokenDto& value) -> std::optional<std::string> {
    if (value.callId.empty() || rtc_naming::callKindOf(value.callId) != rtc_naming::CallKind::Invalid)
      return std::nullopt;
    return "callId must be rtc-<32 hex> or call-<id>";
  })
  CUSTOM_LAMBDA(resume, [](const RtcTokenDto& value) -> std::optional<std::string> {
    if (value.resumeIsBoolean)
      return std::nullopt;
    return "resume must be a boolean";
  })
  IS_IN_OPTIONAL(mode, "duplex", "half")
  END_VALIDATION()
  return dto;
}
