#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <validation/validation_dsl.hxx>
#include <string>

struct CreateCameraDto
{
  std::string name;
  std::string manufacturer;
  std::string model;
  std::string ip;
  int32_t port{554};
  std::string username;
  std::string password;
  std::string cloudUsername;
  std::string cloudPassword;
  std::string driver;
  std::string icon;
  std::string recordMode;
  std::optional<int64_t> retentionDays;
  std::optional<std::string> streamPath;
  std::optional<std::string> subStreamPath;
  std::optional<std::string> catalogId;

  static CreateCameraDto fromJson(const Json::Value& json);
};
