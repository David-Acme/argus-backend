#pragma once

#include <json/value.h>

class NatsBus;

// Single subscriber of the `argus.*.v1.change` wildcard that routes by the
// concrete subject: legacy change events fan straight out to /sync, while
// camera-domain events from argus-camera insert the audit row verbatim into
// the gateway's audit substrate BEFORE fanning the resulting row out
// (Ruling Y) — so both the online WebSocket replay and the offline
// audit-cursor catch-up observe the same insert order.
namespace camera_fan_out
{
void handleCameraChange(const Json::Value& json);
void subscribeChangeFanOut(NatsBus& bus);
} // namespace camera_fan_out