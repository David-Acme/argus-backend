#include "speech-render.hxx"

#include "speech-cues.hxx"

#include <text/iso-time.hxx>
#include <text/spoken-time.hxx>

#include <array>
#include <json/json.h>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace turn::speech
{

namespace
{

struct ExampleSet
{
  std::array<ExampleLine, 2> es;
  std::array<ExampleLine, 2> en;
};

constexpr ExampleSet kExampleAskSlot{
    .es = {ExampleLine{"ponme un recordatorio", "¿Para cuándo quieres que te lo recuerde?"},
           ExampleLine{"anótame una cita", "¿Para qué día la quieres?"}},
    .en = {ExampleLine{"set a reminder", "When should I remind you?"},
           ExampleLine{"jot a thought down", "What should I write in the note?"}}};

constexpr ExampleSet kExampleConfirm{
    .es = {ExampleLine{"borra la nota de la compra", "¿Quieres que borre «Nota de la compra»?"},
           ExampleLine{"elimina la nota de la compra", "¿Confirmas que borre «Nota de la compra»?"}},
    .en = {ExampleLine{"delete the shopping note", "Shall I delete “Shopping note”?"},
           ExampleLine{"cancel my dentist visit", "Do you want me to cancel “Dentist visit”?"}}};

constexpr ExampleSet kExampleChoose{
    .es = {ExampleLine{"guarda esto", "¿Quieres un evento en el calendario o una tarea?"},
           ExampleLine{"anota esta idea", "¿Lo quieres como evento o como tarea?"}},
    .en = {ExampleLine{"save this", "Should I add an event on the calendar or a task?"},
           ExampleLine{"keep this", "Would you like an event on the calendar or a task?"}}};

constexpr ExampleSet kExampleDone{
    .es = {ExampleLine{"agenda la reunión", "Listo, quedó para el jueves a las cinco."},
           ExampleLine{"agenda la reunión mañana", "Listo, ya quedó agendada."}},
    .en = {ExampleLine{"schedule the meeting", "Done, it is set for Thursday at five."},
           ExampleLine{"note the task", "Done, I jotted it down."}}};

constexpr ExampleSet kExampleRefused{
    .es = {ExampleLine{"cancela la cita vieja", "No encontré esa cita, así que no la cancelé. ¿Quieres que la busque de otra forma?"},
           ExampleLine{"cancela la cita del lunes", "No encontré esa cita, así que no la cancelé."}},
    .en = {ExampleLine{"cancel the old meeting", "I could not find that meeting, so I did not cancel it. Want me to look another way?"},
           ExampleLine{"delete that", "That was not there, so nothing was deleted."}}};

constexpr ExampleSet kExampleOffer{
    .es = {ExampleLine{"apunta una idea en tareas", "El módulo de tareas está apagado. ¿Quieres que lo active?"},
           ExampleLine{"guarda una idea en tareas", "El módulo de tareas está apagado. ¿Quieres que lo active?"}},
    .en = {ExampleLine{"keep an idea in tasks", "The tasks module is off. Shall I turn it on?"},
           ExampleLine{"remind me to call", "That module is off. Do you want me to switch it on?"}}};

constexpr ExampleSet kExampleDeclined{
    .es = {ExampleLine{"no gracias", "Vale, lo dejo así."},
           ExampleLine{"mejor lo dejamos", "Entendido, lo dejo así."}},
    .en = {ExampleLine{"no thanks", "Alright, I will leave it."},
           ExampleLine{"never mind that", "No problem, I will not touch it."}}};

constexpr ExampleSet kExampleUnactionable{
    .es = {ExampleLine{"ponme música", "Eso no lo puedo hacer, pero puedo ponerte un recordatorio."},
           ExampleLine{"traduce este texto", "Eso no lo puedo hacer."}},
    .en = {ExampleLine{"play me some music", "I cannot do that, but I can set a reminder for you."},
           ExampleLine{"sing me a song", "That is not something I can do."}}};

constexpr ExampleSet kExampleMisunderstood{
    .es = {ExampleLine{"mmm", "No te entendí. ¿Me lo repites?"},
           ExampleLine{"este...", "No te entendí. ¿Otra vez?"}},
    .en = {ExampleLine{"er", "I did not catch that. Could you say it again?"},
           ExampleLine{"hrm", "Sorry, I missed that. Say it once more?"}}};

const ExampleSet& examplesOf(const Act& act)
{
  if (std::holds_alternative<AskSlot>(act))
    return kExampleAskSlot;
  if (std::holds_alternative<Confirm>(act))
    return kExampleConfirm;
  if (std::holds_alternative<Choose>(act))
    return kExampleChoose;
  if (std::holds_alternative<Done>(act))
    return kExampleDone;
  if (std::holds_alternative<Refused>(act))
    return kExampleRefused;
  if (std::holds_alternative<Offer>(act))
    return kExampleOffer;
  if (std::holds_alternative<Declined>(act))
    return kExampleDeclined;
  if (std::holds_alternative<Unactionable>(act))
    return kExampleUnactionable;
  return kExampleMisunderstood;
}

struct ActionPhrase
{
  std::string_view tool;
  std::string_view es;
  std::string_view en;
};

constexpr std::array<ActionPhrase, 9> kActionPhrases{{
    {.tool = "calendar.create_event", .es = "agendar el evento", .en = "schedule the event"},
    {.tool = "calendar.cancel_event", .es = "cancelar el evento", .en = "cancel the event"},
    {.tool = "task.create", .es = "anotar la tarea", .en = "add the task"},
    {.tool = "task.complete", .es = "marcar la tarea como hecha", .en = "mark the task as done"},
    {.tool = "project.create", .es = "crear el proyecto", .en = "create the project"},
    {.tool = "memory.remind", .es = "poner el recordatorio", .en = "set the reminder"},
    {.tool = "memory.remember", .es = "guardarlo en la memoria", .en = "save it to memory"},
    {.tool = "app.show_camera", .es = "mostrar la cámara", .en = "show the camera"},
    {.tool = "app.set_guard_mode", .es = "cambiar la vigilancia", .en = "change the guard mode"},
}};

std::string_view actionPhrase(std::string_view tool, std::string_view lang)
{
  for (const ActionPhrase& phrase : kActionPhrases)
    if (phrase.tool == tool)
      return lang == "en" ? phrase.en : phrase.es;
  return slotActionName({.tool = tool, .lang = lang});
}

std::string_view appNote(std::string_view lang)
{
  return lang == "en" ? "App note:" : "Nota de la app:";
}

std::string_view reasonText(AskReason reason, std::string_view lang)
{
  const bool english = lang == "en";
  switch (reason) {
    case AskReason::Missing:
      return english ? "it is still needed" : "hay que pedirlo";
    case AskReason::AmbiguousDate:
      return english ? "there are two possible dates" : "hay dos fechas posibles";
    case AskReason::DatePassed:
      return english ? "that time already passed today" : "esa hora ya pasó hoy";
    case AskReason::BeyondRange:
      return english ? "that date is too far away" : "esa fecha queda muy lejos";
    case AskReason::ProjectChoice:
      return english ? "a project must be chosen" : "hay que elegir un proyecto";
    case AskReason::ProjectName:
      return english ? "the project needs a name" : "hay que ponerle nombre al proyecto";
    case AskReason::ProjectNoneYet:
      return english ? "there are no projects yet" : "todavía no hay proyectos";
  }
  return english ? "it is still needed" : "hay que pedirlo";
}

struct ReasonCode
{
  std::string_view code;
  std::string_view lang;
};

std::string_view reasonCodeText(ReasonCode input)
{
  const std::string_view code = input.code;
  const bool english = input.lang == "en";
  if (code == "not_found")
    return english ? "not found" : "no lo encontré";
  if (code == "project_create_unavailable")
    return english ? "cannot create projects" : "no puedo crear proyectos";
  if (code == "no_matching_action")
    return english ? "no matching action" : "no hay una acción que corresponda";
  return {};
}

std::string_view relativeWord(DateKind kind, std::string_view lang)
{
  const bool english = lang == "en";
  switch (kind) {
    case DateKind::Today:
      return english ? "today" : "hoy";
    case DateKind::Tomorrow:
      return english ? "tomorrow" : "mañana";
    case DateKind::DayAfterTomorrow:
      return english ? "the day after tomorrow" : "pasado mañana";
    default:
      return {};
  }
}

std::string slotLabelOf(std::string_view slot, std::string_view lang)
{
  return std::string(slotLabel({.key = slot, .lang = lang}));
}

std::string optionNameOf(std::string_view option, std::string_view lang)
{
  const std::string_view label = optionLabel({.key = option, .lang = lang});
  return label.empty() ? std::string(option) : std::string(label);
}

std::string argValue(const Json::Value& value, std::string_view lang, int64_t now)
{
  if (value.isString()) {
    const auto at = iso_time::parse(value.asString());
    if (at)
      return dateSurface({.kind = DateKind::CalendarDate, .epoch = *at}, lang, now);
    return value.asString();
  }
  if (value.isBool())
    return value.asBool() ? (lang == "en" ? "yes" : "sí") : "no";
  if (value.isInt64())
    return std::to_string(value.asInt64());
  return {};
}

std::string joinNamed(const std::vector<std::string>& keys, std::string_view lang, std::string_view sep)
{
  std::string out;
  for (const std::string& key : keys) {
    if (!out.empty())
      out += sep;
    out += optionNameOf(key, lang);
  }
  return out;
}

std::string quoted(const std::string& value, std::string_view lang)
{
  return lang == "en" ? "\"" + value + "\"" : "«" + value + "»";
}

std::string askSituation(const AskSlot& ask, std::string_view lang, int64_t now)
{
  const bool english = lang == "en";
  std::string line = english ? "missing " : "falta ";
  line += slotLabelOf(ask.slot, lang);
  if (!ask.options.empty())
    line += (english ? " (one of: " : " (uno de: ") + joinNamed(ask.options, lang, ", ") + ")";
  else if (ask.reason != AskReason::Missing)
    line += " (" + std::string(reasonText(ask.reason, lang)) + ")";
  line += '.';
  std::string known;
  if (ask.knownArgs.isObject())
    for (const std::string& key : ask.knownArgs.getMemberNames()) {
      const std::string label = slotLabelOf(key, lang);
      const std::string value = argValue(ask.knownArgs[key], lang, now);
      if (label.empty() || value.empty())
        continue;
      if (!known.empty())
        known += ", ";
      known += label + ": " + value;
    }
  if (!known.empty())
    line += (english ? " (known: " : " (ya tienes: ") + known + ")";
  if (!ask.dates.empty()) {
    line += english ? " Dates: " : " Fechas: ";
    bool first = true;
    for (const DatePart& part : ask.dates) {
      if (!first)
        line += english ? " or " : " o ";
      line += dateSurface(part, lang, now);
      first = false;
    }
  }
  return line;
}

std::string confirmSituation(const Confirm& confirm, std::string_view lang, int64_t now)
{
  const bool english = lang == "en";
  Json::Value args = confirm.args;
  args.removeMember("confirmation");
  std::string primary;
  std::string extra;
  for (const std::string& key : args.getMemberNames()) {
    const std::string value = argValue(args[key], lang, now);
    if (value.empty())
      continue;
    if (primary.empty()) {
      primary = value;
      continue;
    }
    const std::string label = slotLabelOf(key, lang);
    if (label.empty())
      continue;
    if (!extra.empty())
      extra += "; ";
    extra += label + ": " + value;
  }
  std::string line = english ? "to " : "para ";
  line += actionPhrase(confirm.action, lang);
  if (!primary.empty())
    line += " " + quoted(primary, lang);
  line += english ? " the user must still confirm" : " falta la confirmación del usuario";
  if (confirm.irreversible)
    line += english ? "; it cannot be undone" : "; no se puede deshacer";
  line += '.';
  if (!extra.empty())
    line += " " + extra + ".";
  if (!confirm.module.empty())
    line += (english ? " Module: " : " Módulo: ") + confirm.module + ".";
  return line;
}

std::string chooseSituation(const Choose& choose, std::string_view lang)
{
  const bool english = lang == "en";
  std::string line = english ? "the user must still choose between " : "falta que el usuario elija entre ";
  line += joinNamed(choose.options, lang, english ? " or " : " o ");
  line += '.';
  return line;
}

bool isDigitAt(std::string_view text, std::size_t at)
{
  return at < text.size() && text[at] >= '0' && text[at] <= '9';
}

bool isoStampAt(std::string_view text, std::size_t at)
{
  if (at + 16 > text.size())
    return false;
  for (const std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U})
    if (!isDigitAt(text, at + index))
      return false;
  return text[at + 4] == '-' && text[at + 7] == '-' && text[at + 10] == 'T' && text[at + 13] == ':';
}

std::size_t isoStampEnd(std::string_view text, std::size_t at)
{
  std::size_t end = at + 16;
  if (end + 2 < text.size() && text[end] == ':' && isDigitAt(text, end + 1) && isDigitAt(text, end + 2))
    end += 3;
  if (end + 1 < text.size() && text[end] == '.' && isDigitAt(text, end + 1))
    while (isDigitAt(text, end + 1))
      ++end;
  if (end < text.size() && text[end] == 'Z')
    return end + 1;
  if (end + 5 < text.size() && (text[end] == '+' || text[end] == '-') && isDigitAt(text, end + 1) &&
      isDigitAt(text, end + 2) && text[end + 3] == ':' && isDigitAt(text, end + 4) && isDigitAt(text, end + 5))
    return end + 6;
  return end;
}

std::string withoutIsoStamp(std::string text, std::string_view replacement)
{
  for (std::size_t at = 0; at + 16 <= text.size(); ++at)
    if (isoStampAt(text, at)) {
      text.replace(at, isoStampEnd(text, at) - at, replacement);
      break;
    }
  return text;
}

std::string doneSituation(const Done& done)
{
  if (done.readback.empty())
    return std::string(done.fact);
  return withoutIsoStamp(std::string(done.fact), done.readback);
}

std::string refusedSituation(const Refused& refused, std::string_view lang)
{
  const bool english = lang == "en";
  std::string line = english ? "it was not done: " : "no se hizo: ";
  line += actionPhrase(refused.tool, lang);
  const std::string_view reason = reasonCodeText({.code = refused.reason, .lang = lang});
  if (!reason.empty())
    line += " (" + std::string(reason) + ")";
  else if (!refused.reason.empty())
    line += " (" + refused.reason + ")";
  line += '.';
  return line;
}

std::string offerSituation(const Offer& offer, std::string_view lang)
{
  const bool english = lang == "en";
  const std::string name = offer.name.empty() ? (english ? "that module" : "ese módulo") : offer.name;
  std::string line = english ? "the " + name + " module is off." : "el módulo " + name + " está apagado.";
  if (!offer.facts.empty())
    line += " " + offer.facts;
  if (!offer.pendingIntent.empty())
    line += (english ? " Pending: " : " Pendiente: ") + offer.pendingIntent + ".";
  return line;
}

std::string situationFor(const Act& act, std::string_view lang, int64_t now)
{
  const std::string body = std::visit(
      [&](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AskSlot>)
          return askSituation(value, lang, now);
        else if constexpr (std::is_same_v<T, Confirm>)
          return confirmSituation(value, lang, now);
        else if constexpr (std::is_same_v<T, Choose>)
          return chooseSituation(value, lang);
        else if constexpr (std::is_same_v<T, Done>)
          return doneSituation(value);
        else if constexpr (std::is_same_v<T, Refused>)
          return refusedSituation(value, lang);
        else if constexpr (std::is_same_v<T, Offer>)
          return offerSituation(value, lang);
        else if constexpr (std::is_same_v<T, Declined>)
          return lang == "en" ? "the user said no." : "el usuario dijo que no.";
        else if constexpr (std::is_same_v<T, Unactionable>)
          return lang == "en" ? "no Argus action applies." : "no hay una acción de Argus que corresponda.";
        else
          return lang == "en" ? "the user was not understood." : "no se entendió al usuario.";
      },
      act);
  return std::string(appNote(lang)) + " " + body;
}

}

std::string dateSurface(const DatePart& part, std::string_view lang, int64_t now)
{
  const auto day = [&](spoken_time::Day dayKind) {
    return spoken_time::day({.epoch = part.epoch, .now = now, .lang = lang, .day = dayKind, .bare = false});
  };
  switch (part.kind) {
    case DateKind::Today:
    case DateKind::Tomorrow:
    case DateKind::DayAfterTomorrow: {
      const std::string_view word = relativeWord(part.kind, lang);
      const std::string weekday = spoken_time::weekdayName({.epoch = part.epoch, .now = now, .lang = lang});
      return lang == "en" ? std::string(word) + ", which is a " + weekday
                          : std::string(word) + " " + weekday;
    }
    case DateKind::Weekday:
      return day(spoken_time::Day::Weekday);
    case DateKind::CalendarDate:
      return day(spoken_time::Day::Relative);
    case DateKind::Clock:
      return spoken_time::clock({.epoch = part.epoch, .now = now, .lang = lang});
  }
  return {};
}

std::string actTail(const RenderInput& input)
{
  if (input.speech.acts.empty())
    return {};
  std::string tail;
  for (const Act& act : input.speech.acts) {
    if (!tail.empty())
      tail += '\n';
    tail += situationFor(act, input.speech.lang, input.speech.now);
  }
  if (input.examples)
    for (const ExampleLine& line : exampleLines(input.speech.acts.back(), input.speech.lang)) {
      tail += "\n";
      tail += line.first;
      tail += " -> ";
      tail += line.second;
    }
  if (!input.contextBlock.empty()) {
    tail += '\n';
    tail += input.contextBlock;
  }
  return tail;
}

std::span<const ExampleLine> exampleLines(const Act& act, std::string_view lang)
{
  const ExampleSet& set = examplesOf(act);
  return lang == "en" ? std::span<const ExampleLine>(set.en) : std::span<const ExampleLine>(set.es);
}

}
