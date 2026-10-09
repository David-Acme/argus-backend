#include "speech-guard.hxx"

#include "speech-cues.hxx"
#include "speech-render.hxx"

#include <feature/llm/services/tools/reply-claims.hxx>

#include <text/iso-time.hxx>
#include <text/name-match.hxx>
#include <text/spoken-time.hxx>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace turn::speech
{

namespace
{

using Words = std::vector<std::string>;

constexpr std::size_t kLongestSpoken = 60;
constexpr std::size_t kMaxNamedOptions = 3;
constexpr std::string_view kInvertedQuestion = "\xC2\xBF";
constexpr std::array<std::string_view, 2> kMoreEs{"o otro", "u otro"};
constexpr std::array<std::string_view, 2> kMoreEn{"or another", "or other"};

Words wordsOf(std::string_view text)
{
  const std::string folded = text_norm::folded(std::string(text));
  Words words;
  std::size_t at = 0;
  while (at < folded.size()) {
    const std::size_t end = std::min(folded.find(' ', at), folded.size());
    if (end > at)
      words.push_back(folded.substr(at, end - at));
    at = end + 1;
  }
  return words;
}

bool phraseIn(const Words& words, std::string_view phrase)
{
  const Words needle = wordsOf(phrase);
  if (needle.empty() || needle.size() > words.size())
    return false;
  for (std::size_t at = 0; at + needle.size() <= words.size(); ++at) {
    bool hit = true;
    for (std::size_t index = 0; index < needle.size() && hit; ++index)
      hit = words.at(at + index) == needle.at(index);
    if (hit)
      return true;
  }
  return false;
}

bool stemIn(const Words& words, std::span<const std::string_view> stems)
{
  for (const std::string_view stem : stems)
    for (const std::string& word : words)
      if (word.starts_with(stem))
        return true;
  return false;
}

bool asks(std::string_view text)
{
  return text.find(kInvertedQuestion) != std::string_view::npos || text.find('?') != std::string_view::npos;
}

std::string subjectSurface(const Json::Value& args)
{
  for (const char* key : {"title", "name", "text", "query"}) {
    const Json::Value& value = args[key];
    if (value.isString() && !value.asString().empty() && value.asString().size() <= kLongestSpoken)
      return value.asString();
  }
  return {};
}

GuardVerdict checkAsk(const AskSlot& ask, const Words& words, std::string_view lang, int64_t now)
{
  const std::span<const std::string_view> cues = cuesFor({.slot = ask.slot, .lang = lang});
  if (!cues.empty() && !std::ranges::any_of(cues, [&words](std::string_view cue) { return phraseIn(words, cue); }))
    return GuardVerdict::SlotNotAsked;
  for (const std::string& known : ask.knownArgs.getMemberNames())
    if (std::ranges::any_of(cuesFor({.slot = known, .lang = lang}),
                            [&words](std::string_view cue) { return phraseIn(words, cue); }))
      return GuardVerdict::SlotReasked;
  if (!ask.dates.empty() &&
      !std::ranges::any_of(ask.dates, [&](const DatePart& part) { return phraseIn(words, dateSurface(part, lang, now)); }))
    return GuardVerdict::SlotNotAsked;
  if (!ask.options.empty()) {
    const std::size_t named = std::min(ask.options.size(), kMaxNamedOptions);
    for (std::size_t index = 0; index < named; ++index)
      if (!phraseIn(words, ask.options.at(index)))
        return GuardVerdict::OptionsIncomplete;
    if (ask.options.size() > kMaxNamedOptions) {
      const std::span<const std::string_view> more = lang == "en" ? std::span<const std::string_view>(kMoreEn)
                                                                 : std::span<const std::string_view>(kMoreEs);
      if (!std::ranges::any_of(more, [&words](std::string_view phrase) { return phraseIn(words, phrase); }))
        return GuardVerdict::OptionsIncomplete;
    }
  }
  return GuardVerdict::Pass;
}

GuardVerdict checkConfirm(const Confirm& confirm, const Words& words, std::string_view lang, int64_t now)
{
  if (!stemIn(words, actionMarkers({.tool = confirm.action, .lang = lang})))
    return GuardVerdict::ActionUnnamed;
  if (!confirm.module.empty() && !phraseIn(words, confirm.module))
    return GuardVerdict::ArgumentMissing;
  const std::string subject = subjectSurface(confirm.args);
  if (!subject.empty() && !phraseIn(words, subject))
    return GuardVerdict::ArgumentMissing;
  for (const char* key : {"starts_at", "due_at"}) {
    const Json::Value& value = confirm.args[key];
    if (!value.isString())
      continue;
    const auto at = iso_time::parse(value.asString());
    if (!at)
      continue;
    const spoken_time::When when{.epoch = *at, .now = now, .lang = lang, .day = spoken_time::Day::Relative};
    if (!phraseIn(words, spoken_time::day(when)) && !phraseIn(words, spoken_time::clock(when)))
      return GuardVerdict::ArgumentMissing;
    break;
  }
  return GuardVerdict::Pass;
}

GuardVerdict checkChoose(const Choose& choose, const Words& words, std::string_view lang)
{
  for (const std::string& option : choose.options)
    if (!stemIn(words, actionMarkers({.tool = option, .lang = lang})))
      return GuardVerdict::OptionsIncomplete;
  return GuardVerdict::Pass;
}

GuardVerdict checkDone(const Done& done, const Words& words)
{
  if (done.readback.empty())
    return GuardVerdict::Pass;
  std::string core = text_norm::folded(done.readback);
  for (const std::string_view lead : {"el ", "on "})
    if (core.starts_with(lead))
      core.erase(0, lead.size());
  return phraseIn(words, core) ? GuardVerdict::Pass : GuardVerdict::ArgumentMissing;
}

GuardVerdict checkOffer(const Offer& offer, const Words& words)
{
  return !offer.name.empty() && !phraseIn(words, offer.name) ? GuardVerdict::SlotNotAsked : GuardVerdict::Pass;
}

}

GuardVerdict check(const GuardInput& input)
{
  if (!input.callsConfirmed && reply_claims::claimsCall({.text = input.reply, .lang = input.speech.lang}))
    return GuardVerdict::ClaimedWithoutTool;
  if (!input.wrote &&
      reply_claims::claimsDone(
          {.text = input.reply, .asked = input.asked, .appOnly = input.opened, .lang = input.speech.lang}))
    return GuardVerdict::ClaimedWithoutTool;
  const Words words = wordsOf(input.reply);
  const bool question = asks(input.reply);
  for (const Act& act : input.speech.acts) {
    GuardVerdict verdict = GuardVerdict::Pass;
    if (const auto* ask = std::get_if<AskSlot>(&act)) {
      if (!question)
        return GuardVerdict::NotAQuestion;
      verdict = checkAsk(*ask, words, input.speech.lang, input.speech.now);
    }
    else if (const auto* confirm = std::get_if<Confirm>(&act)) {
      if (!question)
        return GuardVerdict::NotYesOrNo;
      verdict = checkConfirm(*confirm, words, input.speech.lang, input.speech.now);
    }
    else if (const auto* choose = std::get_if<Choose>(&act)) {
      if (!question)
        return GuardVerdict::NotAQuestion;
      verdict = checkChoose(*choose, words, input.speech.lang);
    }
    else if (const auto* done = std::get_if<Done>(&act))
      verdict = checkDone(*done, words);
    else if (const auto* offer = std::get_if<Offer>(&act)) {
      if (!question)
        return GuardVerdict::SlotNotAsked;
      verdict = checkOffer(*offer, words);
    }
    if (verdict != GuardVerdict::Pass)
      return verdict;
  }
  return GuardVerdict::Pass;
}

std::string_view verdictName(GuardVerdict verdict)
{
  switch (verdict) {
    case GuardVerdict::Pass:
      return "pass";
    case GuardVerdict::NotAQuestion:
      return "not_a_question";
    case GuardVerdict::SlotNotAsked:
      return "slot_not_asked";
    case GuardVerdict::SlotReasked:
      return "slot_reasked";
    case GuardVerdict::OptionsIncomplete:
      return "options_incomplete";
    case GuardVerdict::ActionUnnamed:
      return "action_unnamed";
    case GuardVerdict::ArgumentMissing:
      return "argument_missing";
    case GuardVerdict::NotYesOrNo:
      return "not_yes_or_no";
    case GuardVerdict::ClaimedWithoutTool:
      return "claimed_without_tool";
  }
  return "pass";
}

std::string_view feedback(GuardVerdict verdict, std::string_view lang)
{
  const bool english = lang == "en";
  switch (verdict) {
    case GuardVerdict::NotAQuestion:
      return english ? "Your reply was not a question. Ask it, in your own words."
                     : "Tu respuesta no era una pregunta. Hazla pregunta, en tus palabras.";
    case GuardVerdict::SlotNotAsked:
      return english ? "Your reply did not ask for the missing value. Ask for it now, with a question."
                     : "Tu respuesta no pedía el dato que falta. Pídelo ahora, con una pregunta.";
    case GuardVerdict::SlotReasked:
      return english ? "You already knew that value; do not ask it again. Ask only for what is missing."
                     : "Ya sabías ese dato; no lo vuelvas a preguntar. Pregunta solo por lo que falta.";
    case GuardVerdict::OptionsIncomplete:
      return english ? "You did not name every option. Name them, or the first three and \"or another\"."
                     : "No nombraste todas las opciones. Nómbralas, o las tres primeras y \"u otro\".";
    case GuardVerdict::ActionUnnamed:
      return english ? "You did not say which action it is. Name it and ask whether they confirm."
                     : "No dijiste qué acción es. Dila y pregunta si lo confirma.";
    case GuardVerdict::ArgumentMissing:
      return english ? "You left out a value that is indicated. Say it with those words and add nothing else."
                     : "No dijiste un dato de los que se indican. Dilo con esas palabras y no añadas nada más.";
    case GuardVerdict::NotYesOrNo:
      return english ? "Your reply was not a yes-or-no question. Ask it that way."
                     : "Tu respuesta no era una pregunta de sí o no. Pregúntalo así.";
    case GuardVerdict::ClaimedWithoutTool:
      return english ? "You said something was done that was not done. Correct it without saying it was done."
                     : "Dijiste que se hizo algo que no se hizo. Corrígelo sin decir que se hizo.";
    case GuardVerdict::Pass:
      break;
  }
  return {};
}

}
