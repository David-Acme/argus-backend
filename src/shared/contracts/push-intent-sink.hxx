#pragma once

#include <json/value.h>
#include <string>

// One notification push intent: a display-only mirror of an already-persisted
// notification row, fanned to the device through the tunnel transport (F5-5).
// It is not a persisted change and never reaches /sync; it never carries
// alarm/siren semantics.
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

// Installed once at boot behind [push] enabled (default off); publication is
// best-effort and never fails the notification emit.
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
