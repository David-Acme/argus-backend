#include "voice-sample-dto.hxx"

#include <algorithm>
#include <cctype>
#include <drogon/utils/Utilities.h>

namespace
{
bool base64Shaped(const std::string& value)
{
  return !value.empty() && value.size() % 4 == 0 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isalnum(character) != 0 || character == '+' ||
                  character == '/' || character == '=';
         });
}
}

VoiceSampleDto VoiceSampleDto::fromJson(const Json::Value& json)
{
  VoiceSampleDto dto;
  if (json.isMember("audio") && json["audio"].isString())
    dto.audio = json["audio"].asString();
  if (json.isMember("challengeId") && json["challengeId"].isString())
    dto.challengeId = json["challengeId"].asString();
  if (json.isMember("phrase") && json["phrase"].isIntegral())
    dto.phrase = json["phrase"].asInt64();

  START_VALIDATION(VoiceSampleDto, dto)
  CUSTOM_LAMBDA(audio,
                [](const VoiceSampleDto& value) -> std::optional<std::string> {
                  if (value.audio.size() > kMaxEncodedBytes)
                    return "audio must not exceed 8MB of base64";
                  if (!base64Shaped(value.audio))
                    return "audio must be a base64-encoded WAV file";
                  return std::nullopt;
                })
  MAX_LENGTH(challengeId, 128)
  BETWEEN(phrase, 0, 9)
  END_VALIDATION()
  return dto;
}

std::string VoiceSampleDto::wav() const
{
  return drogon::utils::base64Decode(audio);
}
