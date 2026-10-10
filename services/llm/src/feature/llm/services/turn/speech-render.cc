#include "speech-render.hxx"

#include "speech-cues.hxx"

#include <text/iso-time.hxx>
#include <text/spoken-time.hxx>

#include <array>
#include <json/json.h>
#include <span>
#include <utility>

namespace turn::speech
{

namespace
{

struct Pair
{
  std::string_view es;
  std::string_view en;
};

constexpr Pair kAskSlot{
    .es = "Formula UNA pregunta, en tus palabras, para pedirle al usuario el dato que falta. No digas que hiciste "
          "nada. No repitas una pregunta que ya hiciste.",
    .en = "Ask ONE question, in your own words, for the detail you still need. Do not claim anything was done. Do not "
          "repeat a question you already asked."};
constexpr Pair kConfirm{
    .es = "Pregúntale al usuario, en una frase, si confirma la acción que se indica. Di qué acción y con qué datos. "
          "No digas que ya se hizo.",
    .en = "Ask the user, in one sentence, whether they confirm the action. Name the action and its arguments. Do not "
          "say it was done."};
constexpr Pair kChoose{.es = "Pregúntale en tus palabras cuál de las opciones quiere. Nombra las opciones.",
                       .en = "Ask, in your own words, which option they want. Name the options."};
constexpr Pair kDone{.es = "Dile al usuario en una o dos frases cortas lo que se hizo, usando solo estos datos.",
                     .en = "Tell the user in one or two short sentences what was done, using only this."};
constexpr Pair kRefused{
    .es = "Dile al usuario con naturalidad que no se hizo; si hay forma de seguir, ofrécela. No digas que se hizo.",
    .en = "Tell the user naturally that it was not done; if there is a way forward, offer it. Do not say it was done."};
constexpr Pair kOffer{.es = "Dile que está apagado y ofrécele lo que se indica; espera su respuesta.",
                      .en = "Say it is off and offer what is described; wait for their answer."};
constexpr Pair kDeclined{.es = "El usuario dijo que no. Acéptalo con naturalidad en una frase corta.",
                         .en = "The user said no. Accept it naturally in one short sentence."};
constexpr Pair kUnactionable{
    .es = "No hay acción de Argus que corresponda. Díselo y, si puedes, ofrece otra forma.",
    .en = "No Argus action matches. Say so and, if you can, offer another way."};
constexpr Pair kMisunderstood{.es = "No lo has entendido. Pide que lo repita, una vez, en tus palabras.",
                              .en = "You did not catch that. Ask them to repeat once, in your own words."};

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

std::string_view pick(const Pair& pair, std::string_view lang)
{
  return lang == "en" ? pair.en : pair.es;
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

std::string_view kindWord(const Act& act, std::string_view lang)
{
  const bool english = lang == "en";
  return std::visit(
      [english](const auto& value) -> std::string_view {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AskSlot>)
          return english ? "question" : "pregunta";
        else if constexpr (std::is_same_v<T, Confirm>)
          return english ? "confirmation" : "confirmación";
        else if constexpr (std::is_same_v<T, Choose>)
          return english ? "choice" : "elección";
        else if constexpr (std::is_same_v<T, Done>)
          return english ? "result" : "resultado";
        else if constexpr (std::is_same_v<T, Refused>)
          return english ? "refusal" : "no se pudo";
        else if constexpr (std::is_same_v<T, Offer>)
          return english ? "offer" : "oferta";
        else if constexpr (std::is_same_v<T, Declined>)
          return english ? "the user said no" : "el usuario dijo que no";
        else if constexpr (std::is_same_v<T, Unactionable>)
          return english ? "no action applies" : "sin acción";
        else
          return english ? "not understood" : "no entendido";
      },
      act);
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

std::string oneLine(const Json::Value& value)
{
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["emitUTF8"] = true;
  return Json::writeString(builder, value);
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

Json::Value actJson(const Act& act, std::string_view lang, int64_t now)
{
  Json::Value out(Json::objectValue);
  out["kind"] = std::string(kindWord(act, lang));
  if (const auto* ask = std::get_if<AskSlot>(&act)) {
    out["about"] = slotLabelOf(ask->slot, lang);
    if (ask->reason != AskReason::Missing)
      out["because"] = std::string(reasonText(ask->reason, lang));
    if (ask->knownArgs.isObject())
      for (const std::string& key : ask->knownArgs.getMemberNames()) {
        const std::string label = slotLabelOf(key, lang);
        if (!label.empty())
          out["known"].append(label);
      }
    for (const DatePart& part : ask->dates)
      out["dates"].append(dateSurface(part, lang, now));
    for (const std::string& option : ask->options)
      out["options"].append(optionNameOf(option, lang));
    return out;
  }
  if (const auto* confirm = std::get_if<Confirm>(&act)) {
    out["action"] = std::string(slotActionName({.tool = confirm->action, .lang = lang}));
    Json::Value args = confirm->args;
    args.removeMember("confirmation");
    for (const std::string& key : args.getMemberNames()) {
      const std::string label = slotLabelOf(key, lang);
      if (label.empty())
        continue;
      std::string value = argValue(args[key], lang, now);
      if (!value.empty())
        out["details"][label] = value;
    }
    if (confirm->irreversible)
      out["irreversible"] = true;
    if (!confirm->toolPreview.empty())
      out["preview"] = confirm->toolPreview;
    if (!confirm->module.empty())
      out["module"] = confirm->module;
    return out;
  }
  if (const auto* choose = std::get_if<Choose>(&act)) {
    for (const std::string& option : choose->options)
      out["options"].append(optionNameOf(option, lang));
    return out;
  }
  if (const auto* done = std::get_if<Done>(&act)) {
    out["action"] = std::string(slotActionName({.tool = done->tool, .lang = lang}));
    out["fact"] = done->fact;
    if (!done->readback.empty())
      out["readback"] = done->readback;
    return out;
  }
  if (const auto* refused = std::get_if<Refused>(&act)) {
    out["action"] = std::string(slotActionName({.tool = refused->tool, .lang = lang}));
    const std::string_view reason = reasonCodeText({.code = refused->reason, .lang = lang});
    if (!reason.empty())
      out["because"] = std::string(reason);
    else if (!refused->reason.empty())
      out["because"] = refused->reason;
    return out;
  }
  if (const auto* offer = std::get_if<Offer>(&act)) {
    if (!offer->name.empty())
      out["about"] = offer->name;
    if (!offer->facts.empty())
      out["details"] = offer->facts;
    if (!offer->pendingIntent.empty())
      out["pending"] = offer->pendingIntent;
    return out;
  }
  if (const auto* unactionable = std::get_if<Unactionable>(&act)) {
    const std::string_view reason = reasonCodeText({.code = unactionable->reason, .lang = lang});
    if (!reason.empty())
      out["because"] = std::string(reason);
  }
  return out;
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

std::string_view instructionLine(const Act& act, std::string_view lang)
{
  return std::visit(
      [lang](const auto& value) -> std::string_view {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AskSlot>)
          return pick(kAskSlot, lang);
        else if constexpr (std::is_same_v<T, Confirm>)
          return pick(kConfirm, lang);
        else if constexpr (std::is_same_v<T, Choose>)
          return pick(kChoose, lang);
        else if constexpr (std::is_same_v<T, Done>)
          return pick(kDone, lang);
        else if constexpr (std::is_same_v<T, Refused>)
          return pick(kRefused, lang);
        else if constexpr (std::is_same_v<T, Offer>)
          return pick(kOffer, lang);
        else if constexpr (std::is_same_v<T, Declined>)
          return pick(kDeclined, lang);
        else if constexpr (std::is_same_v<T, Unactionable>)
          return pick(kUnactionable, lang);
        else
          return pick(kMisunderstood, lang);
      },
      act);
}

std::string actTail(const RenderInput& input)
{
  if (input.speech.acts.empty())
    return {};
  std::string tail(instructionLine(input.speech.acts.back(), input.speech.lang));
  for (const ExampleLine& line : exampleLines(input.speech.acts.back(), input.speech.lang)) {
    tail += "\n";
    tail += line.first;
    tail += " -> ";
    tail += line.second;
  }
  for (const Act& act : input.speech.acts) {
    tail += '\n';
    tail += oneLine(actJson(act, input.speech.lang, input.speech.now));
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
