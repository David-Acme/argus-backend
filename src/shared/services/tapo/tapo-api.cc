#include "tapo-api.hxx"

#include <chrono>
#include <drogon/drogon.h>
#include <utility>

namespace
{

Json::Value makeRequest(const std::string& method, const Json::Value& params)
{
  Json::Value request(Json::objectValue);
  request["method"] = method;
  if (!params.isNull())
    request["params"] = params;
  return request;
}

Json::Value nameList(const std::string& group, const std::vector<std::string>& names)
{
  Json::Value list(Json::arrayValue);
  for (const auto& name : names)
    list.append(name);
  Json::Value params(Json::objectValue);
  params[group]["name"] = list;
  return params;
}

std::string switchOf(bool enabled)
{
  return enabled ? "on" : "off";
}

std::optional<bool> switchValue(const Json::Value& node)
{
  if (!node.isString())
    return std::nullopt;
  const std::string value = node.asString();
  if (value == "on")
    return true;
  if (value == "off")
    return false;
  return std::nullopt;
}

const Json::Value& responseAt(const Json::Value& data, Json::ArrayIndex index)
{
  static const Json::Value empty;
  const auto& responses = data["result"]["responses"];
  if (!responses.isArray() || index >= responses.size())
    return empty;
  return responses[index];
}

int64_t nowSeconds()
{
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

Json::Value TapoStatusBatch::toJson() const
{
  Json::Value value(Json::objectValue);
  value["ok"] = ok;
  if (!error.empty())
    value["error"] = error;
  value["model"] = model;
  value["firmware"] = firmware;
  value["hardwareVersion"] = hardwareVersion;
  value["mac"] = mac;
  value["deviceId"] = deviceId;
  if (privacyEnabled)
    value["privacyEnabled"] = *privacyEnabled;
  if (ledEnabled)
    value["ledEnabled"] = *ledEnabled;
  if (motionEnabled)
    value["motionEnabled"] = *motionEnabled;
  if (autoTrackEnabled)
    value["autoTrackEnabled"] = *autoTrackEnabled;
  if (dayNightMode)
    value["dayNightMode"] = *dayNightMode;
  if (deviceTime)
    value["deviceTime"] = static_cast<Json::Int64>(*deviceTime);
  if (clockOffsetSeconds)
    value["clockOffsetSeconds"] = static_cast<Json::Int64>(*clockOffsetSeconds);
  return value;
}

TapoApi::TapoApi(TapoClientConfig config) : client_(std::move(config)) {}

TapoResult TapoApi::connect()
{
  return client_.connect();
}

bool TapoApi::isConnected() const
{
  return client_.isConnected();
}

Json::Value TapoApi::state() const
{
  return client_.state();
}

TapoResult TapoApi::call(const std::string& method, const Json::Value& params)
{
  return client_.batch({makeRequest(method, params)});
}

TapoResult TapoApi::callBatch(const std::vector<Json::Value>& requests)
{
  return client_.batch(requests);
}

TapoResult TapoApi::callRaw(const Json::Value& payload)
{
  return client_.invoke(payload);
}

TapoResult TapoApi::getDeviceInfo()
{
  return call("getDeviceInfo", nameList("device_info", {"basic_info"}));
}

TapoResult TapoApi::getDeviceTime()
{
  Json::Value params(Json::objectValue);
  params["system"]["name"] = "clock_status";
  return call("getClockStatus", params);
}

TapoResult TapoApi::getSdCardStatus()
{
  Json::Value params(Json::objectValue);
  Json::Value table(Json::arrayValue);
  table.append("hd_info");
  params["harddisk_manage"]["table"] = table;
  return call("getSdCardStatus", params);
}

TapoResult TapoApi::getAudioConfig()
{
  return call("getAudioConfig", nameList("audio_config", {"speaker", "microphone"}));
}

TapoResult TapoApi::getPresets()
{
  return call("getPresetConfig", nameList("preset", {"preset"}));
}

TapoResult TapoApi::move(const TapoMoveInput& input)
{
  Json::Value params(Json::objectValue);
  params["motor"]["move"]["x_coord"] = std::to_string(input.x);
  params["motor"]["move"]["y_coord"] = std::to_string(input.y);
  return call("motorMove", params);
}

TapoResult TapoApi::step(const TapoStepInput& input)
{
  Json::Value params(Json::objectValue);
  params["motor"]["movestep"]["direction"] = std::to_string(input.angle);
  return call("motorMoveStep", params);
}

TapoResult TapoApi::gotoPreset(const TapoPresetInput& input)
{
  Json::Value params(Json::objectValue);
  params["preset"]["goto_preset"]["id"] = input.id;
  return call("motorMoveToPreset", params);
}

TapoResult TapoApi::savePreset(const TapoPresetInput& input)
{
  Json::Value params(Json::objectValue);
  params["preset"]["set_preset"]["name"] = input.name;
  params["preset"]["set_preset"]["save_ptz"] = "1";
  return call("addMotorPostion", params);
}

TapoResult TapoApi::deletePreset(const TapoPresetInput& input)
{
  Json::Value ids(Json::arrayValue);
  ids.append(input.id);
  Json::Value params(Json::objectValue);
  params["preset"]["remove_preset"]["id"] = ids;
  return call("deletePreset", params);
}

TapoResult TapoApi::setPrivacy(const TapoPrivacyInput& input)
{
  Json::Value params(Json::objectValue);
  params["lens_mask"]["lens_mask_info"]["enabled"] = switchOf(input.enabled);
  return call("setLensMaskConfig", params);
}

TapoResult TapoApi::setLed(const TapoLedInput& input)
{
  Json::Value params(Json::objectValue);
  params["led"]["config"]["enabled"] = switchOf(input.enabled);
  return call("setLedStatus", params);
}

TapoResult TapoApi::setDayNight(const TapoDayNightInput& input)
{
  Json::Value params(Json::objectValue);
  params["image"]["common"]["inf_type"] = input.mode;
  return call("setDayNightModeConfig", params);
}

TapoResult TapoApi::setMotion(const TapoMotionInput& input)
{
  Json::Value params(Json::objectValue);
  params["motion_detection"]["motion_det"]["enabled"] = switchOf(input.enabled);
  if (input.sensitivity)
    params["motion_detection"]["motion_det"]["digital_sensitivity"] =
        std::to_string(*input.sensitivity);
  return call("setDetectionConfig", params);
}

TapoResult TapoApi::setAutoTrack(const TapoAutoTrackInput& input)
{
  Json::Value params(Json::objectValue);
  params["target_track"]["target_track_info"]["enabled"] = switchOf(input.enabled);
  return call("setTargetTrackConfig", params);
}

TapoResult TapoApi::setAlarm(const TapoAlarmInput& input)
{
  Json::Value params(Json::objectValue);
  params["msg_alarm"]["manual_msg_alarm"]["action"] = input.enabled ? "start" : "stop";
  if (input.durationSeconds)
    params["msg_alarm"]["manual_msg_alarm"]["duration"] =
        std::to_string(*input.durationSeconds);
  if (input.volume)
    params["msg_alarm"]["manual_msg_alarm"]["volume"] = std::to_string(*input.volume);
  return call("setAlertConfig", params);
}

TapoResult TapoApi::searchDetectionList(const TapoEventFilter& filter)
{
  Json::Value params(Json::objectValue);
  auto& search = params["playback"]["search_detection_list"];
  search["start_index"] = filter.startIndex;
  search["start_time"] = static_cast<Json::Int64>(filter.startTime);
  search["end_time"] = static_cast<Json::Int64>(filter.endTime);
  search["end_index"] = filter.endIndex;
  return call("searchDetectionList", params);
}

TapoStatusBatch TapoApi::getStatus()
{
  TapoStatusBatch status;

  const std::vector<Json::Value> requests = {
      makeRequest("getDeviceInfo", nameList("device_info", {"basic_info"})),
      makeRequest("getLensMaskConfig", nameList("lens_mask", {"lens_mask_info"})),
      makeRequest("getLedStatus", nameList("led", {"config"})),
      makeRequest("getDetectionConfig", nameList("motion_detection", {"motion_det"})),
      makeRequest("getTargetTrackConfig",
                  nameList("target_track", {"target_track_info"})),
      makeRequest("getDayNightModeConfig", nameList("image", {"common"})),
      makeRequest("getSdCardStatus", [] {
        Json::Value params(Json::objectValue);
        Json::Value table(Json::arrayValue);
        table.append("hd_info");
        params["harddisk_manage"]["table"] = table;
        return params;
      }()),
      makeRequest("getClockStatus", [] {
        Json::Value params(Json::objectValue);
        params["system"]["name"] = "clock_status";
        return params;
      }())};

  const auto result = callBatch(requests);
  status.raw = result.data;
  if (!result.ok) {
    status.error = result.error;
    return status;
  }

  const auto& info = responseAt(result.data, 0)["result"]["device_info"]["basic_info"];
  status.model = info["device_model"].asString();
  status.firmware = info["sw_version"].asString();
  status.hardwareVersion = info["hw_version"].asString();
  status.mac = info["mac"].asString();
  status.deviceId = info["dev_id"].asString();

  status.privacyEnabled = switchValue(
      responseAt(result.data, 1)["result"]["lens_mask"]["lens_mask_info"]["enabled"]);
  status.ledEnabled =
      switchValue(responseAt(result.data, 2)["result"]["led"]["config"]["enabled"]);
  status.motionEnabled = switchValue(
      responseAt(result.data, 3)["result"]["motion_detection"]["motion_det"]["enabled"]);
  status.autoTrackEnabled =
      switchValue(responseAt(result.data,
                             4)["result"]["target_track"]["target_track_info"]["enabled"]);

  const auto& image = responseAt(result.data, 5)["result"]["image"]["common"]["inf_type"];
  if (image.isString())
    status.dayNightMode = image.asString();

  const auto& clock =
      responseAt(result.data, 7)["result"]["system"]["clock_status"]["seconds_from_1970"];
  if (clock.isIntegral() || clock.isString()) {
    const int64_t deviceTime =
        clock.isString() ? std::strtoll(clock.asString().c_str(), nullptr, 10)
                         : clock.asInt64();
    status.deviceTime = deviceTime;
    status.clockOffsetSeconds = deviceTime - nowSeconds();
  }

  status.ok = true;
  return status;
}
