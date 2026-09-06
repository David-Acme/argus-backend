#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

// Internal wire request (Ruling BP): {image_b64, prompt?, camera_id?}.
// image_b64 is a base64 JPEG — the in-process API takes a cv::Mat, so the
// caller encodes before sending.
struct DescribeImageDto
{
  std::string imageB64;
  std::string prompt;
  std::string cameraId;

  static DescribeImageDto fromJson(const Json::Value& json);
};
