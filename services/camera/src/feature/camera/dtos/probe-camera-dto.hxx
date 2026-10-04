#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>

struct ProbeCameraDto
{
  std::string driver;
  std::string ip;
  int32_t port{554};
  std::string username;
  std::string password;
  std::string cloudUsername;
  std::string cloudPassword;
  std::string streamPath;
  std::string subStreamPath;
  std::optional<int64_t> cameraId;

  static ProbeCameraDto fromJson(const Json::Value& json);
};
