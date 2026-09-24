#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <string>

struct DescribeImageDto
{
  std::string imageB64;
  std::string prompt;
  std::string cameraId;

  static DescribeImageDto fromJson(const Json::Value& json);
};
