#include "time-arguments.hxx"

#include <feature/memory/services/extract/call-time.hxx>
#include <text/iso-time.hxx>

#include <array>
#include <ctime>

namespace time_arguments
{

namespace
{
constexpr std::string_view kDateTime = "date-time";

std::optional<int64_t> resolved(const std::string& text, const std::string& lang, int64_t now)
{
  if (text.empty())
    return std::nullopt;
  const auto found = call_time::resolve({.text = text, .lang = lang, .now = now});
  if (!found)
    return std::nullopt;
  return found->fireAt;
}

bool isDateTime(const Json::Value& property)
{
  return property.isObject() && property["format"].isString() && property["format"].asString() == kDateTime;
}

bool isRequired(const Json::Value& schema, const std::string& name)
{
  for (const auto& entry : schema["required"])
    if (entry.isString() && entry.asString() == name)
      return true;
  return false;
}

std::optional<int64_t> chosen(const Json::Value& given, const std::optional<int64_t>& heard,
                              const NormalizeInput& input)
{
  if (given.isString()) {
    if (const auto iso = iso_time::parse(given.asString()))
      return heard ? heard : iso;
    if (const auto spoken = resolved(given.asString(), input.call.context.lang, input.now))
      return spoken;
  }
  return heard;
}
}

bool needsClock(const argus::mcp::ToolSpec& spec)
{
  const Json::Value& properties = spec.inputSchema["properties"];
  for (const auto& name : properties.getMemberNames())
    if (isDateTime(properties[name]))
      return true;
  return false;
}

std::string clockLine(int64_t now, const std::string& lang)
{
  constexpr std::array<const char*, 7> kEs{"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
  constexpr std::array<const char*, 7> kEn{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  const auto seconds = static_cast<std::time_t>(now);
  std::tm local{};
  localtime_r(&seconds, &local);
  const auto weekday = static_cast<size_t>(local.tm_wday);
  const bool english = lang == "en";
  return std::string(english ? "(Current date and time: " : "(Fecha y hora actuales: ") + iso_time::format(now) + ", " +
         (english ? kEn.at(weekday) : kEs.at(weekday)) + ")";
}

void normalize(const NormalizeInput& input)
{
  tools::ToolCall& call = input.call;
  if (!call.arguments.isObject())
    return;
  const Json::Value& schema = input.spec.inputSchema;
  const Json::Value& properties = schema["properties"];
  for (const auto& name : properties.getMemberNames()) {
    if (!isDateTime(properties[name]))
      continue;
    const bool mandatory = isRequired(schema, name);
    const auto heard = mandatory ? resolved(call.context.utterance, call.context.lang, input.now) : std::nullopt;
    const Json::Value given = call.arguments.isMember(name) ? call.arguments[name] : Json::Value();
    if (const auto at = chosen(given, heard, input))
      call.arguments[name] = iso_time::format(*at);
  }
}

}
