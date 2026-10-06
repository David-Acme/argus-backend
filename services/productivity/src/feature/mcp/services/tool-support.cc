#include "tool-support.hxx"

#include <text/iso-time.hxx>
#include <text/sha256.hxx>

#include <array>
#include <ctime>

namespace productivity_tools
{

namespace
{
constexpr std::array<const char*, 7> kWeekdaysEs{"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
constexpr std::array<const char*, 7> kWeekdaysEn{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr std::array<const char*, 12> kMonthsEs{"enero", "febrero", "marzo", "abril", "mayo", "junio",
                                                "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
constexpr std::array<const char*, 12> kMonthsEn{"January", "February", "March",     "April",   "May",      "June",
                                                "July",    "August",   "September", "October", "November", "December"};
constexpr std::string_view kHex = "0123456789abcdef";
constexpr size_t kKeyDigits = 32;

std::string two(int value)
{
  return (value < 10 ? "0" : "") + std::to_string(value);
}
}

std::string spokenWhen(const SpokenTime& when, std::string_view lang)
{
  const auto seconds = static_cast<std::time_t>(when.epoch);
  std::tm local{};
  localtime_r(&seconds, &local);
  const auto weekday = static_cast<size_t>(local.tm_wday);
  const auto month = static_cast<size_t>(local.tm_mon);
  const bool en = lang == "en";
  std::string out = en ? std::string(kWeekdaysEn.at(weekday)) + ", " + kMonthsEn.at(month) + " " + std::to_string(local.tm_mday)
                       : "el " + std::string(kWeekdaysEs.at(weekday)) + " " + std::to_string(local.tm_mday) + " de " +
                             kMonthsEs.at(month);
  if (when.allDay)
    return out;
  if (en) {
    const int hour = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
    out += " at " + std::to_string(hour) + ":" + two(local.tm_min) + (local.tm_hour < 12 ? " AM" : " PM");
  }
  else {
    out += " a las " + two(local.tm_hour) + ":" + two(local.tm_min);
  }
  return out;
}

std::string idempotencyKey(const IdempotencyInput& input)
{
  argus::hash::Sha256 hash;
  hash.update(input.scope + "|" + std::to_string(input.userId) + "|" + input.subject + "|" + std::to_string(input.at));
  std::string out = "mcp-";
  for (const uint8_t byte : hash.digest()) {
    out.push_back(kHex[byte >> 4U]);
    out.push_back(kHex[byte & 0x0FU]);
    if (out.size() >= kKeyDigits + 4)
      break;
  }
  return out;
}

std::optional<int64_t> timeArgument(const Json::Value& arguments, const char* name)
{
  if (!arguments.isObject() || !arguments[name].isString())
    return std::nullopt;
  return iso_time::parse(arguments[name].asString());
}

argus::mcp::ToolOutcome invalid(const ValidationException& error, const argus::mcp::ToolInvocation& invocation)
{
  std::string fields;
  for (const auto& [field, messages] : error.errors()) {
    if (messages.empty())
      continue;
    fields += (fields.empty() ? "" : "; ") + field + ": " + messages.front();
  }
  return argus::mcp::toolFailure(
      {.text = (argus::mcp::speech::inEnglish(invocation) ? "That is not valid: " : "Eso no es válido: ") + fields, .code = "invalid_arguments"});
}

argus::mcp::ToolOutcome unknownCaller(const argus::mcp::ToolInvocation& invocation)
{
  return argus::mcp::speech::refuse({.invocation = invocation,
                                     .spanish = "No sé quién eres, así que no puedo usar la agenda.",
                                     .english = "I do not know who you are, so I cannot use the agenda."},
                                    "identity_required");
}

}
