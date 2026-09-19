#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

// Internal wire request: {image_b64, prompt?, camera_id?}.
struct DescribeImageDto
{
  std::string imageB64;
  std::string prompt;
  std::string cameraId;

  static DescribeImageDto fromJson(const Json::Value& json);
};
