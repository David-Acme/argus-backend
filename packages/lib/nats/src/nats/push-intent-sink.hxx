#pragma once

#include <json/value.h>
#include <string>

// Display-only push intent mirrored to the device through the tunnel; never a /sync event.
struct PushIntent
{
  int64_t userId{0};
  int64_t notificationId{0};
  std::string type;
  std::string title;
  std::string body;
  int64_t createdAtMs{0};
};

namespace push_intent
{
inline Json::Value toJson(const PushIntent& intent)
{
  Json::Value json(Json::objectValue);
  json["userId"] = Json::Value::Int64(intent.userId);
  json["notificationId"] = Json::Value::Int64(intent.notificationId);
  json["type"] = intent.type;
  json["title"] = intent.title;
  json["body"] = intent.body;
  json["createdAt"] = Json::Value::Int64(intent.createdAtMs);
  return json;
}

// Push-intent sink installed once at boot behind [push] enabled (default off).
class PushIntentSink
{
public:
  virtual ~PushIntentSink() = default;

  virtual void publish(const PushIntent& intent) const = 0;
};

inline const PushIntentSink*& sink()
{
  static const PushIntentSink* instance = nullptr;
  return instance;
}

inline void setSink(const PushIntentSink* value)
{
  sink() = value;
}

inline const PushIntentSink* getSink()
{
  return sink();
}
} // namespace push_intent
