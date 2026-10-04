#include "tapo-motor.hxx"

#include <string>

namespace tapo_motor
{
TapoResult outcomeOf(TapoResult result)
{
  Json::Value outcome(Json::objectValue);
  const Json::Value& responses = result.data["result"]["responses"];
  const int error = !result.ok ? result.errorCode
                    : responses.isArray() && !responses.empty()
                        ? responses[0].get("error_code", 0).asInt()
                        : result.data.get("error_code", 0).asInt();
  if (error == kLockedRotor) {
    outcome["moved"] = false;
    outcome["limit"] = true;
    return TapoResult::success(outcome);
  }
  if (!result.ok)
    return result;
  if (error != 0)
    return TapoResult::failure(
        "the camera refused the move (error " + std::to_string(error) + ")", error);
  outcome["moved"] = true;
  outcome["limit"] = false;
  return TapoResult::success(outcome);
}
}
