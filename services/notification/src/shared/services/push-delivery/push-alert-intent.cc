#include "push-alert-intent.hxx"

namespace push_alert_intent
{

Json::Value build(const Input& input)
{
  Json::Value intent(Json::objectValue);
  intent["intentId"] = input.intentId;
  intent["kind"] = "alert";
  intent["urgency"] = input.urgency;
  intent["notificationId"] = static_cast<Json::Int64>(input.notificationId);
  intent["type"] = input.type;
  intent["title"] = input.title;
  intent["body"] = input.body;
  intent["deepLink"] = "argus://notification?notificationId=" + std::to_string(input.notificationId);
  intent["iat"] = static_cast<Json::Int64>(input.issuedAt);
  intent["exp"] = static_cast<Json::Int64>(input.expiresAt);
  if (!input.episode.empty())
    intent["episode"] = input.episode;
  if (input.ringFollows)
    intent["ringFollows"] = true;
  return intent;
}

}
