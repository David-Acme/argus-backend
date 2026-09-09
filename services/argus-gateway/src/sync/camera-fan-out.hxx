#pragma once

#include <json/value.h>

class NatsBus;

// Routes argus.*.v1.change; camera audits insert before fanning out (Ruling Y).
namespace camera_fan_out
{
void handleCameraChange(const Json::Value& json);
void subscribeChangeFanOut(NatsBus& bus);
} // namespace camera_fan_out
