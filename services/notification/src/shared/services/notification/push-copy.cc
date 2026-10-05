#include "push-copy.hxx"

namespace
{
bool urgent(const std::string& urgency)
{
  return urgency == "critical" || urgency == "time_sensitive";
}

std::string textOf(const Json::Value& data, const char* key)
{
  if (!data.isObject())
    return {};
  const Json::Value& value = data[key];
  return value.isString() ? value.asString() : std::string{};
}
}

PushCopy push_copy::render(const PushCopyInput& input)
{
  const bool english = input.lang == "en";
  if (input.call) {
    if (english)
      return {.title = "Argus is calling you", .body = "Open Argus to answer."};
    return {.title = "Argus te está llamando",
            .body = "Abre Argus para contestar."};
  }
  if (urgent(input.urgency)) {
    if (english)
      return {.title = "Argus",
              .body = "There is an important alert. Open Argus to see it."};
    return {.title = "Argus",
            .body = "Hay un aviso importante. Abre Argus para verlo."};
  }
  if (english)
    return {.title = "Argus", .body = "You have a new notice. Open Argus to see it."};
  return {.title = "Argus", .body = "Tienes un aviso nuevo. Abre Argus para verlo."};
}

Json::Value push_copy::minimalData(const PushDataInput& input)
{
  Json::Value data(Json::objectValue);
  data["notificationId"] = static_cast<Json::Int64>(input.notificationId);
  data["kind"] = textOf(input.data, "kind");
  data["urgency"] = urgencyOf(input.data);
  return data;
}

std::string push_copy::langOf(const Json::Value& data)
{
  return textOf(data, "lang") == "en" ? "en" : "es";
}

std::string push_copy::urgencyOf(const Json::Value& data)
{
  const std::string urgency = textOf(data, "urgency");
  return urgency.empty() ? std::string("active") : urgency;
}
