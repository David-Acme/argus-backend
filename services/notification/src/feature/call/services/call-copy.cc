#include "call-copy.hxx"

#include <array>
#include <ctime>
#include <string_view>
#include <vector>

namespace
{
struct Pair
{
  std::string_view es;
  std::string_view en;
};

std::string pick(const Pair& pair, bool english)
{
  return std::string(english ? pair.en : pair.es);
}

std::string text(const Json::Value& data, const char* key)
{
  const Json::Value& value = data[key];
  return value.isString() ? value.asString() : std::string{};
}

int64_t number(const Json::Value& data, const char* key)
{
  const Json::Value& value = data[key];
  return value.isIntegral() ? value.asInt64() : 0;
}

std::string greeting(const std::string& name, bool english)
{
  if (name.empty())
    return english ? "Hi. " : "Hola. ";
  return (english ? "Hi " : "Hola, ") + name + ". ";
}

std::string place(const Json::Value& data)
{
  std::string camera = text(data, "cameraName");
  std::string environment = text(data, "environmentName");
  if (camera.empty())
    return environment;
  if (!environment.empty())
    camera += " (" + environment + ")";
  return camera;
}

std::string who(const Json::Value& data, bool english)
{
  const std::string subject = text(data, "subject");
  const int64_t people = number(data, "people");
  if (subject == "several" && people > 1) {
    return english ? std::to_string(people) + " unknown people"
                   : std::to_string(people) + " personas desconocidas";
  }
  if (subject == "unobserved")
    return pick({.es = "alguien sin identificar", .en = "someone unidentified"},
                english);
  if (subject == "accompanied")
    return pick({.es = "una persona desconocida acompañada",
                 .en = "an unknown person with company"},
                english);
  return pick({.es = "una persona desconocida", .en = "an unknown person"},
              english);
}

std::string reasonClause(const Json::Value& data, bool english)
{
  const Json::Value& reasons = data["reasons"];
  if (!reasons.isArray())
    return {};
  static constexpr std::array<std::pair<std::string_view, Pair>, 7> kOrder{{
      {"weapon", {.es = "y parece llevar un arma", .en = "and seems to carry a weapon"}},
      {"armed", {.es = "con la alarma armada", .en = "while the alarm is armed"}},
      {"nobody_home", {.es = "con la casa vacía", .en = "while nobody is home"}},
      {"night", {.es = "de noche", .en = "at night"}},
      {"after_hours", {.es = "fuera de horario", .en = "after hours"}},
      {"alert_zone", {.es = "en una zona de alerta", .en = "inside an alert zone"}},
      {"several_strangers", {.es = "y no está sola", .en = "and not alone"}},
  }};
  for (const auto& [name, phrase] : kOrder) {
    for (const auto& reason : reasons) {
      if (reason.isString() && reason.asString() == name)
        return ", " + pick(phrase, english);
    }
  }
  return {};
}

std::string actionSentence(const Json::Value& data, bool english)
{
  const std::string action = text(data, "action");
  if (action == "speaker")
    return pick({.es = " Le estoy avisando por el altavoz.",
                 .en = " I am warning them through the speaker."},
                english);
  if (action == "alarm")
    return pick({.es = " He hecho sonar la alarma de la cámara.",
                 .en = " I have sounded the camera alarm."},
                english);
  if (action == "silent_weapon")
    return pick({.es = " No le he dicho nada para no ponerte en riesgo.",
                 .en = " I have said nothing to them, to keep you safe."},
                english);
  if (action == "greeted")
    return pick({.es = " Le he saludado.", .en = " I have greeted them."},
                english);
  if (action == "greeted_no_reply")
    return pick({.es = " Le he saludado y no ha contestado.",
                 .en = " I greeted them and got no answer."},
                english);
  return pick({.es = " Lo estoy vigilando.", .en = " I am watching."}, english);
}

std::string capitalized(std::string value)
{
  if (!value.empty() && value.front() >= 'a' && value.front() <= 'z')
    value.front() = static_cast<char>(value.front() - 'a' + 'A');
  return value;
}

std::string clipped(const std::string& value, std::size_t limit)
{
  if (value.size() <= limit)
    return value;
  std::size_t cut = limit;
  while (cut > 0 &&
         (static_cast<unsigned char>(value[cut]) & 0xC0U) == 0x80U)
    --cut;
  return value.substr(0, cut) + "…";
}

CallCopy guardCopy(const CallCopyInput& input, bool english)
{
  const Json::Value& data = input.data;
  const std::string where = place(data);
  const std::string someone = who(data, english);
  const std::string at = where.empty() ? std::string{}
                                       : (english ? " at " : " en ") + where;
  CallCopy copy;
  copy.title = capitalized(someone) + (where.empty() ? "" : " · " + where);
  copy.summary = capitalized(someone) + at + ".";
  std::string opening = greeting(input.userName, english);
  if (input.trigger == CallTrigger::GuardEscalation)
    opening += pick({.es = "Te llamo porque la situación ha empeorado. ",
                     .en = "I am calling because things got worse. "},
                    english);
  else if (input.trigger == CallTrigger::GuardCritical)
    opening += pick({.es = "Te llamo por algo urgente de la vigilancia. ",
                     .en = "I am calling about something urgent. "},
                    english);
  else
    opening += pick({.es = "Te llamo por la vigilancia. ",
                     .en = "I am calling about the security. "},
                    english);
  opening += (english ? "There is " : "Hay ") + someone + at +
             reasonClause(data, english) + "." + actionSentence(data, english);
  if (number(data, "cameraId") > 0)
    opening += pick({.es = " ¿Quieres que te muestre la cámara?",
                     .en = " Do you want me to show you the camera?"},
                    english);
  copy.openingLine = opening;
  copy.missedTitle =
      pick({.es = "Llamada perdida · ", .en = "Missed call · "}, english) +
      copy.title;
  copy.missedLine = (english ? "I called you because there was " +
                                   someone + at + "."
                             : "Te llamé porque había " + someone + at + ".");
  copy.followupLine = (english ? "Also, there is " : "Además, hay ") +
                      someone + at + ".";
  return copy;
}

CallCopy arrivalCopy(const CallCopyInput& input, bool english)
{
  const Json::Value& data = input.data;
  std::string person = text(data, "personName");
  if (person.empty())
    person = pick({.es = "alguien de casa", .en = "someone from home"},
                  english);
  const std::string where = place(data);
  const std::string at = where.empty() ? std::string{}
                                       : (english ? " at " : " a ") + where;
  CallCopy copy;
  copy.title = (english ? person + " arrived" : "Ha llegado " + person) +
               (where.empty() ? "" : " · " + where);
  copy.summary = copy.title;
  copy.openingLine =
      greeting(input.userName, english) +
      (english ? "Just letting you know that " + person + " arrived" + at + "."
               : "Te aviso de que ha llegado " + person + at + ".");
  copy.missedTitle =
      pick({.es = "Llamada perdida · ", .en = "Missed call · "}, english) +
      copy.title;
  copy.missedLine =
      english ? "I called to tell you that " + person + " arrived" + at + "."
              : "Te llamé para avisarte de que había llegado " + person + at +
                    ".";
  copy.followupLine =
      english ? "Also, " + person + " arrived" + at + "."
              : "Además, ha llegado " + person + at + ".";
  return copy;
}

CallCopy agendaCopy(const CallCopyInput& input, bool english)
{
  const Json::Value& data = input.data;
  const std::string title = clipped(text(data, "title"), 120);
  const std::string quoted = english ? "\"" + title + "\"" : "«" + title + "»";
  CallCopy copy;
  copy.missedTitle = pick({.es = "Llamada perdida · ", .en = "Missed call · "},
                          english);
  if (text(data, "kind") == "agenda_reminder") {
    copy.title = pick({.es = "Recordatorio · ", .en = "Reminder · "}, english) +
                 clipped(title, 48);
    copy.summary = title;
    copy.openingLine =
        greeting(input.userName, english) +
        (english ? "A reminder: " : "Te recuerdo: ") + title + ".";
    copy.missedLine =
        (english ? "I called to remind you: " : "Te llamé para recordarte: ") +
        title + ".";
    copy.followupLine =
        (english ? "Also, a reminder: " : "Además, te recuerdo: ") + title +
        ".";
    copy.missedTitle += copy.title;
    return copy;
  }
  const int64_t startsAt = number(data, "startsAt");
  const std::string clock = startsAt > 0 ? call_copy::clockTime(startsAt) : "";
  const bool now = startsAt > 0 && startsAt - input.now <= 60;
  const std::string location = clipped(text(data, "location"), 60);
  const std::string atPlace =
      location.empty() ? std::string{}
                       : (english ? ", at " : ", en ") + location;
  copy.title = pick({.es = "Agenda · ", .en = "Agenda · "}, english) +
               clipped(title, 48);
  copy.summary = clock.empty() ? quoted
                               : quoted + (english ? " at " : " a las ") + clock;
  std::string sentence;
  if (now || clock.empty())
    sentence = english ? quoted + " is starting now" + atPlace + "."
                       : "Ahora empieza " + quoted + atPlace + ".";
  else
    sentence = english ? "At " + clock + " you have " + quoted + atPlace + "."
                       : "A las " + clock + " tienes " + quoted + atPlace + ".";
  copy.openingLine = greeting(input.userName, english) +
                     (english ? "A reminder from your agenda. "
                              : "Te llamo por tu agenda. ") +
                     sentence;
  copy.missedLine =
      (english ? "I called to remind you of " : "Te llamé para recordarte ") +
      copy.summary + ".";
  copy.followupLine = (english ? "Also, " : "Además, ") +
                      (now || clock.empty()
                           ? (english ? quoted + " is starting now."
                                      : "ahora empieza " + quoted + ".")
                           : (english ? "at " + clock + " you have " + quoted +
                                            "."
                                      : "a las " + clock + " tienes " +
                                            quoted + "."));
  copy.missedTitle += copy.title;
  return copy;
}

CallCopy assistantCopy(const CallCopyInput& input, bool english)
{
  const std::string topic = clipped(text(input.data, "topic"), 200);
  CallCopy copy;
  copy.title = pick({.es = "Recordatorio · ", .en = "Reminder · "}, english) +
               clipped(topic, 48);
  copy.summary = topic;
  copy.openingLine =
      greeting(input.userName, english) +
      (english ? "You asked me to call you to remind you: "
               : "Me pediste que te llamara para recordarte: ") +
      topic + ".";
  copy.missedTitle =
      pick({.es = "Llamada perdida · ", .en = "Missed call · "}, english) +
      copy.title;
  copy.missedLine =
      (english ? "I called to remind you: " : "Te llamé para recordarte: ") +
      topic + ".";
  copy.followupLine =
      (english ? "Also, you asked me to remind you: "
               : "Además, me pediste recordarte: ") +
      topic + ".";
  return copy;
}
}

std::string_view call_copy::normalizeLang(std::string_view lang)
{
  return lang == "en" ? "en" : "es";
}

std::string call_copy::clockTime(int64_t epochSeconds)
{
  const auto seconds = static_cast<std::time_t>(epochSeconds);
  std::tm local{};
  localtime_r(&seconds, &local);
  std::array<char, 8> buffer{};
  const std::size_t written =
      std::strftime(buffer.data(), buffer.size(), "%H:%M", &local);
  return {buffer.data(), written};
}

CallCopy call_copy::render(const CallCopyInput& input)
{
  const bool english = normalizeLang(input.lang) == "en";
  switch (input.trigger) {
    case CallTrigger::GuardCritical:
    case CallTrigger::GuardIntruder:
    case CallTrigger::GuardEscalation:
      return guardCopy(input, english);
    case CallTrigger::GuardArrival:
      return arrivalCopy(input, english);
    case CallTrigger::Agenda:
      return agendaCopy(input, english);
    case CallTrigger::Assistant:
      return assistantCopy(input, english);
  }
  return assistantCopy(input, english);
}

std::string call_copy::joinFollowups(const std::string& opening,
                                     const std::vector<std::string>& followups)
{
  std::string line = opening;
  for (const auto& followup : followups) {
    if (followup.empty())
      continue;
    if (!line.empty())
      line += ' ';
    line += followup;
  }
  return line;
}
