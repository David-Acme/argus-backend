#pragma once

#include <feature/camera-control/services/aim-settings.hxx>

#include <json/value.h>
#include <string>

struct CameraAimDto
{
  AimSettingsPatch patch;
  std::string sectorsError;
  std::string flagsError;

  static CameraAimDto fromJson(const Json::Value& json);
};

struct CameraLeaseDto
{
  std::string action;

  static CameraLeaseDto fromJson(const Json::Value& json);
};
