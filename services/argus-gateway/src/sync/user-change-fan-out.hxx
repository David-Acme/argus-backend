#pragma once

#include <json/value.h>

// Audit events insert verbatim before fanning out (Ruling Y); plain pass through.
namespace user_change_fan_out
{
void handleUserChange(const Json::Value& json);
} // namespace user_change_fan_out
