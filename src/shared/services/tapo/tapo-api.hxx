#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/services/tapo/tapo-client.hxx>
#include <string>
#include <vector>

struct TapoMoveInput
{
  int64_t x{0};
  int64_t y{0};
};

struct TapoStepInput
{
  int64_t angle{0};
};

struct TapoPresetInput
{
  std::string id;
  std::string name;
};

struct TapoLedInput
{
  bool enabled{true};
};

struct TapoPrivacyInput
{
  bool enabled{false};
};

struct TapoDayNightInput
{
  std::string mode{"auto"};
};

struct TapoMotionInput
{
  bool enabled{true};
  std::optional<int> sensitivity;
};

struct TapoAutoTrackInput
{
  bool enabled{false};
};

struct TapoAlarmInput
{
  bool enabled{true};
  std::optional<int> durationSeconds;
  std::optional<int> volume;
};

struct TapoEventFilter
{
  int64_t startTime{0};
  int64_t endTime{0};
  int startIndex{0};
  int endIndex{999};
};

struct TapoStatusBatch
{
  bool ok{false};
  std::string error;
  std::string model;
  std::string firmware;
  std::string hardwareVersion;
  std::string mac;
  std::string deviceId;
  std::optional<bool> privacyEnabled;
  std::optional<bool> ledEnabled;
  std::optional<bool> motionEnabled;
  std::optional<bool> autoTrackEnabled;
  std::optional<std::string> dayNightMode;
  std::optional<int64_t> deviceTime;
  std::optional<int64_t> clockOffsetSeconds;
  Json::Value raw;

  Json::Value toJson() const;
};

class TapoApi
{
public:
  explicit TapoApi(TapoClientConfig config);

  TapoResult connect();
  bool isConnected() const;
  Json::Value state() const;

  TapoResult getDeviceInfo();
  TapoResult getDeviceTime();
  TapoResult getSdCardStatus();
  TapoResult getAudioConfig();
  TapoResult getPresets();
  TapoStatusBatch getStatus();

  TapoResult move(const TapoMoveInput& input);
  TapoResult step(const TapoStepInput& input);
  TapoResult gotoPreset(const TapoPresetInput& input);
  TapoResult savePreset(const TapoPresetInput& input);
  TapoResult deletePreset(const TapoPresetInput& input);

  TapoResult setPrivacy(const TapoPrivacyInput& input);
  TapoResult setLed(const TapoLedInput& input);
  TapoResult setDayNight(const TapoDayNightInput& input);
  TapoResult setMotion(const TapoMotionInput& input);
  TapoResult setAutoTrack(const TapoAutoTrackInput& input);
  TapoResult setAlarm(const TapoAlarmInput& input);

  TapoResult searchDetectionList(const TapoEventFilter& filter);

  TapoResult call(const std::string& method, const Json::Value& params);
  TapoResult callBatch(const std::vector<Json::Value>& requests);
  TapoResult callRaw(const Json::Value& payload);

private:
  TapoClient client_;
};
