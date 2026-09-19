#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct UpdateCameraDto
{
  std::optional<std::string> name;
  std::optional<std::string> manufacturer;
  std::optional<std::string> model;
  std::optional<std::string> ip;
  std::optional<int32_t> port;
  std::optional<std::string> username;
  std::optional<std::string> password;
  std::optional<std::string> cloudUsername;
  std::optional<std::string> cloudPassword;
  std::optional<std::string> driver;
  std::optional<std::string> icon;
  std::optional<std::string> recordMode;
  std::optional<int64_t> retentionDays;
  std::optional<bool> isEnabled;

  static UpdateCameraDto fromJson(const Json::Value& json);
};
