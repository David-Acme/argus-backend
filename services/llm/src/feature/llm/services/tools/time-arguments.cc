#include "time-arguments.hxx"

#include <feature/memory/services/extract/call-time.hxx>
#include <text/iso-time.hxx>
#include <text/name-match.hxx>

#include <algorithm>
#include <array>
#include <format>
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

bool asksAboutTime(std::string_view utterance)
{
  constexpr std::array<std::string_view, 10> kQuestionsEs{"que hora es", "que hora tienes", "que horas son",
                                                          "a que hora", "dime la hora", "que dia es",
                                                          "que dia estamos", "que fecha es", "es tarde", "es temprano"};
  constexpr std::array<std::string_view, 11> kQuestionsEn{"what time", "what s the time", "the time is it",
                                                           "do you have the time", "tell me the time", "what day is",
                                                           "what s the day", "what date", "what s the date", "is it late",
                                                           "is it early"};
  const std::string folded = text_norm::folded(std::string(utterance));
  const auto asked = [&folded](const auto& questions) {
    return std::ranges::any_of(questions, [&folded](std::string_view question) {
      return folded.find(question) != std::string::npos;
    });
  };
  return asked(kQuestionsEs) || asked(kQuestionsEn);
}

std::string clockLine(int64_t now, const std::string& lang)
{
  constexpr std::array<const char*, 7> kEs{"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
  constexpr std::array<const char*, 7> kEn{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  constexpr std::array<const char*, 12> kEsMonths{"enero", "febrero", "marzo", "abril", "mayo", "junio",
                                                  "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
  constexpr std::array<const char*, 12> kEnMonths{"January", "February", "March", "April", "May", "June",
                                                  "July", "August", "September", "October", "November", "December"};
  const auto seconds = static_cast<std::time_t>(now);
  std::tm local{};
  localtime_r(&seconds, &local);
  const auto weekday = static_cast<size_t>(local.tm_wday);
  const auto month = static_cast<size_t>(local.tm_mon);
  const std::string day = std::to_string(local.tm_mday);
  const std::string year = std::to_string(local.tm_year + 1900);
  const std::string clock = std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
  if (lang == "en")
    return std::string("Reference, mention it only if asked for the date or time: today is ") + kEn.at(weekday) + ", " +
           kEnMonths.at(month) + " " + day + ", " + year + ", and it is " + clock + ".";
  return std::string("Referencia, menciónala solo si te preguntan la fecha o la hora: hoy es ") + kEs.at(weekday) + " " +
         day + " de " + kEsMonths.at(month) + " de " + year + " y son las " + clock + ".";
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
