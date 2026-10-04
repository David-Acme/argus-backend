#pragma once

#include <json/value.h>
#include <shared/schemas/camera/camera-schema.hxx>
#include <string>

namespace camera_capabilities
{
Json::Value of(const CameraSchema& camera);
std::string listOf(const Json::Value& capabilities);
}
