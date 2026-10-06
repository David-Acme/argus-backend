#include "response-privacy-dto.hxx"

namespace
{
Json::Value stateJson(const PrivacyState& state)
{
  Json::Value json(Json::objectValue);
  json["decided"] = state.decided;
  json["noticeVersion"] = static_cast<Json::Int64>(state.noticeVersion);
  json["current"] = state.decided && state.noticeVersion >= kPrivacyNoticeVersion;
  json["decidedAt"] = state.decided
                          ? Json::Value(static_cast<Json::Int64>(state.decidedAt))
                          : Json::Value();
  json["updatedAt"] = state.decided
                          ? Json::Value(static_cast<Json::Int64>(state.updatedAt))
                          : Json::Value();
  json["choices"] = state.choices.toJson();
  json["effective"] = state.effective.toJson();
  return json;
}

Json::Value applicableJson(bool surveillanceActive)
{
  Json::Value json(Json::objectValue);
  json["presence"] = surveillanceActive;
  json["faceCameras"] = surveillanceActive;
  json["voiceLearning"] = true;
  json["cameraAudio"] = surveillanceActive;
  return json;
}
}

Json::Value ResponsePrivacyDto::toJson() const
{
  Json::Value json = stateJson(view.state);
  json["applicable"] = applicableJson(surveillanceActive);
  json["currentNoticeVersion"] = static_cast<Json::Int64>(kPrivacyNoticeVersion);
  json["household"] = view.household.toJson();
  return json;
}

Json::Value ResponsePrivacyDirectoryDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["applicable"] = applicableJson(surveillanceActive);
  json["currentNoticeVersion"] = static_cast<Json::Int64>(kPrivacyNoticeVersion);
  json["household"] = directory.household.toJson();
  json["household"]["visitorRecognition"] = directory.visitorRecognition;
  json["visitorAcknowledgedAt"] =
      directory.visitorAcknowledgedAt
          ? Json::Value(static_cast<Json::Int64>(*directory.visitorAcknowledgedAt))
          : Json::Value();
  json["householdUpdatedAt"] =
      directory.householdUpdatedAt
          ? Json::Value(static_cast<Json::Int64>(*directory.householdUpdatedAt))
          : Json::Value();
  Json::Value users(Json::arrayValue);
  for (const auto& entry : directory.users) {
    Json::Value user = stateJson(entry.state);
    user["userId"] = static_cast<Json::Int64>(entry.userId);
    users.append(std::move(user));
  }
  json["users"] = std::move(users);
  return json;
}
