#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <json/value.h>
#include <string>

struct MaintenanceVoiceRequest
{
  int64_t userId{0};
  std::string role;
  std::string lang;
  std::string camera;
  int64_t minutes{0};
};

struct MaintenanceVoiceReply
{
  bool ok{false};
  std::string text;
  std::string code;
  Json::Value structured{Json::objectValue};
};

using MaintenanceVoice = std::function<drogon::Task<MaintenanceVoiceReply>(MaintenanceVoiceRequest)>;

struct MaintenanceVoicePorts
{
  MaintenanceVoice start{};
  MaintenanceVoice end{};
  MaintenanceVoice status{};
};
