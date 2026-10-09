#include "speech-render.hxx"

#include <text/iso-time.hxx>
#include <text/spoken-time.hxx>

#include <array>
#include <json/json.h>

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
    .en = "Ask ONE question, in your own words, for the missing value. Do not claim anything was done. Do not repeat "
          "a question you already asked."};
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

std::string_view pick(const Pair& pair, std::string_view lang)
{
  return lang == "en" ? pair.en : pair.es;
}

std::string_view reasonText(AskReason reason)
{
  switch (reason) {
    case AskReason::Missing:
      return "missing";
    case AskReason::AmbiguousDate:
      return "ambiguous_date";
    case AskReason::DatePassed:
      return "date_passed";
    case AskReason::BeyondRange:
      return "beyond_range";
    case AskReason::ProjectChoice:
      return "project_choice";
    case AskReason::ProjectName:
      return "project_name";
    case AskReason::ProjectNoneYet:
      return "project_none_yet";
  }
  return "missing";
}

std::string_view dateKindName(DateKind kind)
{
  switch (kind) {
    case DateKind::Today:
      return "today";
    case DateKind::Tomorrow:
      return "tomorrow";
    case DateKind::DayAfterTomorrow:
      return "day_after_tomorrow";
    case DateKind::Weekday:
      return "weekday";
    case DateKind::CalendarDate:
      return "calendar_date";
    case DateKind::Clock:
      return "clock";
  }
  return "calendar_date";
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

Json::Value actJson(const Act& act, std::string_view lang, int64_t now)
{
  Json::Value out(Json::objectValue);
  out["kind"] = std::string(actName(act));
  if (const auto* ask = std::get_if<AskSlot>(&act)) {
    out["slot"] = ask->slot;
    out["tool"] = ask->tool;
    out["reason"] = std::string(reasonText(ask->reason));
    if (!ask->knownArgs.isNull() && ask->knownArgs.isObject() && ask->knownArgs.size() > 0)
      out["known"] = ask->knownArgs;
    for (const DatePart& part : ask->dates) {
      Json::Value date(Json::objectValue);
      date["kind"] = std::string(dateKindName(part.kind));
      date["iso"] = iso_time::format(part.epoch);
      date["surface"] = dateSurface(part, lang, now);
      out["dates"].append(std::move(date));
    }
    for (const std::string& option : ask->options)
      out["options"].append(option);
    return out;
  }
  if (const auto* confirm = std::get_if<Confirm>(&act)) {
    out["action"] = confirm->action;
    Json::Value args = confirm->args;
    args.removeMember("confirmation");
    out["args"] = std::move(args);
    out["irreversible"] = confirm->irreversible;
    if (!confirm->toolPreview.empty())
      out["preview"] = confirm->toolPreview;
    if (!confirm->module.empty())
      out["module"] = confirm->module;
    return out;
  }
  if (const auto* choose = std::get_if<Choose>(&act)) {
    for (const std::string& option : choose->options)
      out["options"].append(option);
    return out;
  }
  if (const auto* done = std::get_if<Done>(&act)) {
    out["tool"] = done->tool;
    out["fact"] = done->fact;
    if (!done->readback.empty())
      out["readback"] = done->readback;
    return out;
  }
  if (const auto* refused = std::get_if<Refused>(&act)) {
    out["tool"] = refused->tool;
    out["reason"] = refused->reason;
    return out;
  }
  if (const auto* offer = std::get_if<Offer>(&act)) {
    out["module"] = offer->module;
    out["name"] = offer->name;
    if (!offer->facts.empty())
      out["facts"] = offer->facts;
    if (!offer->pendingIntent.empty())
      out["pending_intent"] = offer->pendingIntent;
    return out;
  }
  if (const auto* unactionable = std::get_if<Unactionable>(&act))
    out["reason"] = unactionable->reason;
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

}
