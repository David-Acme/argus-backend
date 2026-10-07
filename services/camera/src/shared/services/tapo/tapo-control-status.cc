#include "tapo-control-status.hxx"

#include <chrono>

int tapo_control::retryAfterSeconds(const TapoControlStatus& status, int64_t nowMs)
{
  if (status.retryAtMs <= nowMs)
    return 0;
  const int64_t remaining = status.retryAtMs - nowMs;
  return static_cast<int>((remaining + 999) / 1000);
}

Json::Value tapo_control::toJson(const TapoControlStatus& status, int64_t nowMs)
{
  const bool scheduled = status.state == TapoControlState::LockedOut ||
                         status.state == TapoControlState::Refused ||
                         status.state == TapoControlState::Unreachable;
  Json::Value json(Json::objectValue);
  json["state"] = tapoControlStateToString(status.state);
  json["credential"] = status.credential;
  json["retryAfterSeconds"] = scheduled ? retryAfterSeconds(status, nowMs) : 0;
  json["retryAt"] = Json::Int64(scheduled ? status.retryAtMs : 0);
  json["code"] = status.code;
  json["message"] = status.message;
  return json;
}

int64_t tapo_control::systemNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
