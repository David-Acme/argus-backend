#include "slots.hxx"

#include "slot-lexicon.hxx"

#include <feature/llm/services/tools/module-command.hxx>
#include <feature/llm/services/tools/time-arguments.hxx>
#include <feature/memory/services/extract/call-time-words.hxx>

#include <text/iso-time.hxx>
#include <text/text-norm.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <ctime>

namespace slots
{

namespace
{
constexpr std::string_view kPunctuation = ",.;!?\xC2\xBF\xC2\xA1\"'()\xC2\xAB\xC2\xBB";
constexpr std::size_t kNamingReach = 3;
constexpr std::size_t kProjectReach = 8;
constexpr std::size_t kLongestTitle = 120;

struct Tokens
{
  std::vector<std::string> raw;
  std::vector<std::string> folded;

  [[nodiscard]] std::size_t size() const { return raw.size(); }
};

std::string foldToken(const std::string& raw)
{
  std::string folded = text_norm::stripAccents(raw);
  std::string out;
  for (const char c : folded)
    if (std::isalnum(static_cast<unsigned char>(c)) != 0)
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string trimmed(std::string text)
{
  while (!text.empty() && kPunctuation.find(text.front()) != std::string_view::npos)
    text.erase(text.begin());
  while (!text.empty() && kPunctuation.find(text.back()) != std::string_view::npos)
    text.pop_back();
  return text;
}

Tokens split(std::string_view text)
{
  Tokens tokens;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n", at);
    if (begin == std::string_view::npos)
      break;
    const std::size_t end = std::min(text.find_first_of(" \t\r\n", begin), text.size());
    std::string raw = trimmed(std::string(text.substr(begin, end - begin)));
    std::string folded = foldToken(raw);
    if (!folded.empty()) {
      tokens.raw.push_back(std::move(raw));
      tokens.folded.push_back(std::move(folded));
    }
    at = end;
  }
  return tokens;
}

bool phraseAt(const Tokens& tokens, std::size_t at, std::string_view phrase)
{
  std::size_t index = at;
  std::size_t begin = 0;
  while (begin <= phrase.size()) {
    const std::size_t end = std::min(phrase.find(' ', begin), phrase.size());
    if (index >= tokens.size() || tokens.folded[index] != phrase.substr(begin, end - begin))
      return false;
    ++index;
    begin = end + 1;
  }
  return true;
}

std::size_t phraseSize(std::string_view phrase)
{
  return static_cast<std::size_t>(std::ranges::count(phrase, ' ') + 1);
}

std::size_t own(slot_lexicon::Group group, const Tokens& tokens, std::size_t at)
{
  std::size_t longest = 0;
  for (const auto& table : slot_lexicon::tables())
    for (const std::string_view phrase : table.groups[static_cast<std::size_t>(group)])
      if (phraseAt(tokens, at, phrase))
        longest = std::max(longest, phraseSize(phrase));
  return longest;
}

std::size_t shared(module_command::Group group, const Tokens& tokens, std::size_t at)
{
  return module_command::phraseLength(group, tokens.folded, at);
}

bool isNumber(const std::string& word)
{
  return !word.empty() && std::ranges::all_of(word, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

std::size_t skipWhile(const Tokens& tokens, std::size_t from, const std::vector<module_command::Group>& shareds,
                      const std::vector<slot_lexicon::Group>& owns)
{
  std::size_t at = from;
  while (at < tokens.size()) {
    std::size_t step = 0;
    for (const auto group : shareds)
      step = std::max(step, shared(group, tokens, at));
    for (const auto group : owns)
      step = std::max(step, own(group, tokens, at));
    if (step == 0)
      break;
    at += step;
  }
  return at;
}

std::size_t afterTrigger(const Tokens& tokens)
{
  std::size_t at = skipWhile(tokens, 0, {module_command::Group::Filler}, {slot_lexicon::Group::Modal});
  std::size_t trigger = 0;
  for (const auto group : {module_command::Group::NewMark, module_command::Group::CreateVerb, module_command::Group::StrongCreate,
                           module_command::Group::CompleteVerb, module_command::Group::CompleteStrong,
                           module_command::Group::CancelVerb, module_command::Group::MarkVerb,
                           module_command::Group::ProjectVerb})
    trigger = std::max(trigger, shared(group, tokens, at));
  if (trigger == 0 && shared(module_command::Group::LeadAgenda, tokens, at) > 0)
    trigger = 1;
  at += trigger;
  at = skipWhile(tokens, at, {}, {slot_lexicon::Group::Clitic, slot_lexicon::Group::Preposition, slot_lexicon::Group::Determiner});
  const auto nouns = std::vector<module_command::Group>{module_command::Group::CalendarNoun, module_command::Group::TaskNoun,
                                                        module_command::Group::ProjectNoun};
  std::size_t noun = 0;
  for (const auto group : nouns)
    noun = std::max(noun, shared(group, tokens, at));
  if (noun > 0) {
    at += noun;
    at = skipWhile(tokens, at, {}, {slot_lexicon::Group::Preposition, slot_lexicon::Group::Determiner});
    const std::size_t project = shared(module_command::Group::ProjectNoun, tokens, at);
    if (project > 0) {
      for (std::size_t probe = at + project; probe < std::min(tokens.size(), at + kProjectReach); ++probe)
        if (tokens.folded[probe] == "para" || tokens.folded[probe] == "to") {
          at = probe + 1;
          break;
        }
    }
  }
  for (std::size_t probe = at; probe < std::min(tokens.size(), at + kNamingReach); ++probe)
    if (own(slot_lexicon::Group::Naming, tokens, probe) > 0) {
      at = probe + 1;
      break;
    }
  return skipWhile(tokens, at, {}, {slot_lexicon::Group::Connector, slot_lexicon::Group::Determiner, slot_lexicon::Group::Preposition});
}

struct Span
{
  std::size_t begin{0};
  std::size_t end{0};
};

std::vector<Span> timeRuns(const Tokens& tokens, std::size_t from)
{
  std::vector<bool> filler(tokens.size(), false);
  std::vector<bool> anchor(tokens.size(), false);
  for (std::size_t at = from; at < tokens.size(); ++at) {
    const std::size_t size = own(slot_lexicon::Group::TimeAnchor, tokens, at);
    if (size > 0)
      for (std::size_t part = 0; part < size; ++part)
        anchor[at + part] = true;
    if (isNumber(tokens.folded[at]) || own(slot_lexicon::Group::TimeFiller, tokens, at) > 0)
      filler[at] = true;
  }
  std::vector<Span> runs;
  std::size_t at = from;
  while (at < tokens.size()) {
    if (!anchor[at]) {
      ++at;
      continue;
    }
    std::size_t begin = at;
    while (begin > from && (filler[begin - 1] || anchor[begin - 1]))
      --begin;
    std::size_t end = at;
    while (end < tokens.size() && (filler[end] || anchor[end]))
      ++end;
    runs.push_back({.begin = begin, .end = end});
    at = end;
  }
  return runs;
}

bool createsItem(std::string_view tool)
{
  return tool == "calendar.create_event" || tool == "task.create" || tool == "project.create";
}

std::string joined(const Tokens& tokens, const std::vector<bool>& dropped, std::size_t from)
{
  std::string out;
  for (std::size_t at = from; at < tokens.size(); ++at) {
    if (dropped[at])
      continue;
    if (!out.empty())
      out += ' ';
    out += tokens.raw[at];
  }
  return out;
}

std::string withoutEdges(const Tokens& tokens, std::vector<bool>& dropped, std::size_t from)
{
  const auto edge = [&tokens](std::size_t at) {
    return own(slot_lexicon::Group::Connector, tokens, at) > 0 || own(slot_lexicon::Group::Determiner, tokens, at) > 0 ||
           own(slot_lexicon::Group::Preposition, tokens, at) > 0 || own(slot_lexicon::Group::Clitic, tokens, at) > 0;
  };
  std::size_t first = from;
  while (first < tokens.size() && (dropped[first] || edge(first)))
    dropped[first++] = true;
  std::size_t last = tokens.size();
  while (last > first && (dropped[last - 1] || (edge(last - 1) && !isNumber(tokens.folded[last - 1]))))
    dropped[--last] = true;
  return joined(tokens, dropped, from);
}
}

std::string capitalized(std::string text)
{
  if (!text.empty() && std::islower(static_cast<unsigned char>(text.front())) != 0)
    text.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
  return text;
}

std::optional<std::string> RuleText::extract(const TextRequest& request) const
{
  if (request.field != "title" && request.field != "name")
    return std::nullopt;
  std::string text(request.utterance);
  bool colon = false;
  if (const std::size_t at = text.find(':'); at != std::string::npos && at + 1 < text.size()) {
    text = text.substr(at + 1);
    colon = true;
  }
  const Tokens tokens = split(text);
  if (tokens.size() == 0)
    return std::nullopt;
  const std::size_t from = colon ? 0 : afterTrigger(tokens);
  std::vector<bool> dropped(tokens.size(), false);
  if (createsItem(request.tool))
    for (const Span& run : timeRuns(tokens, from))
      for (std::size_t at = run.begin; at < run.end; ++at)
        dropped[at] = true;
  std::string title = withoutEdges(tokens, dropped, from);
  if (title.empty() || title.size() > kLongestTitle)
    return std::nullopt;
  return capitalized(std::move(title));
}

std::optional<std::string> answerText(std::string_view utterance)
{
  const Tokens tokens = split(utterance);
  std::size_t at = skipWhile(tokens, 0, {module_command::Group::Filler}, {slot_lexicon::Group::Modal, slot_lexicon::Group::AnswerPrefix});
  std::vector<bool> dropped(tokens.size(), false);
  const std::string answer = withoutEdges(tokens, dropped, at);
  if (answer.empty() || answer.size() > kLongestTitle)
    return std::nullopt;
  return capitalized(answer);
}

bool namesOther(std::string_view utterance)
{
  const Tokens tokens = split(utterance);
  for (std::size_t at = 0; at < tokens.size(); ++at)
    if (own(slot_lexicon::Group::Other, tokens, at) > 0)
      return true;
  return false;
}

bool namesNewOne(std::string_view utterance)
{
  const Tokens tokens = split(utterance);
  for (std::size_t at = 0; at < tokens.size(); ++at)
    if (own(slot_lexicon::Group::NewOne, tokens, at) > 0)
      return true;
  return false;
}

Choice choose(std::string_view utterance, const std::vector<std::string>& options)
{
  const Tokens heard = split(utterance);
  std::size_t best = 0;
  std::vector<std::size_t> hits;
  for (std::size_t index = 0; index < options.size(); ++index) {
    const Tokens words = split(options[index]);
    if (words.size() == 0)
      continue;
    const bool held = std::ranges::all_of(words.folded, [&heard](const std::string& word) {
      return std::ranges::find(heard.folded, word) != heard.folded.end();
    });
    if (!held)
      continue;
    if (words.size() > best) {
      best = words.size();
      hits = {index};
    }
    else if (words.size() == best) {
      hits.push_back(index);
    }
  }
  if (hits.size() == 1)
    return {.kind = ChoiceKind::Chosen, .name = options[hits.front()]};
  return {.kind = hits.empty() ? ChoiceKind::Unknown : ChoiceKind::Ambiguous, .name = {}};
}

std::optional<std::string> nameGiven(std::string_view utterance)
{
  const Tokens tokens = split(utterance);
  for (std::size_t at = 0; at < tokens.size(); ++at) {
    std::size_t size = own(slot_lexicon::Group::Naming, tokens, at);
    if (size == 0)
      size = own(slot_lexicon::Group::AnswerPrefix, tokens, at);
    if (size == 0)
      continue;
    std::vector<bool> dropped(tokens.size(), false);
    const std::string name = withoutEdges(tokens, dropped, at + size);
    if (name.empty() || name.size() > kLongestTitle)
      return std::nullopt;
    return capitalized(name);
  }
  return std::nullopt;
}

CallReading dayReading(const DayHeard& heard)
{
  return call_time::read({.text = heard.utterance, .lang = heard.lang, .now = heard.now});
}

std::optional<std::size_t> chooseDay(const DayAnswer& answer)
{
  const call_time::Tokens heard = call_time::tokenize(call_time::fold(answer.utterance).text);
  std::vector<std::tm> days;
  for (const std::string& value : answer.values) {
    const auto at = iso_time::parse(value);
    if (!at)
      return std::nullopt;
    const auto seconds = static_cast<std::time_t>(*at);
    std::tm local{};
    localtime_r(&seconds, &local);
    days.push_back(local);
  }
  const auto pick = [&days](const std::vector<bool>& hit) -> std::optional<std::size_t> {
    std::optional<std::size_t> only;
    for (std::size_t index = 0; index < days.size(); ++index) {
      if (!hit[index])
        continue;
      if (only)
        return std::nullopt;
      only = index;
    }
    return only;
  };
  std::vector<bool> byWeekday(days.size(), false);
  std::vector<bool> byNumber(days.size(), false);
  std::vector<bool> byOrdinal(days.size(), false);
  std::vector<bool> byRelative(days.size(), false);
  const std::array<std::vector<std::string_view>, 3> relativeWords{{{"hoy", "today", "tonight"}, {"manana", "tomorrow"}, {"pasado", "after"}}};
  for (std::size_t at = 0; at < heard.size(); ++at) {
    if (answer.relative >= 0 && answer.relative < 3 && days.size() > 1) {
      const auto& words = relativeWords.at(static_cast<std::size_t>(answer.relative));
      if (std::ranges::any_of(words, [&](std::string_view word) { return call_time::wordAt(heard, at, word); }))
        byRelative[1] = true;
    }
    const int weekday = call_time::weekdayOf(heard[at].text);
    const auto number = call_time::dayNumberAt(heard, at);
    for (std::size_t index = 0; index < days.size(); ++index) {
      byWeekday[index] = byWeekday[index] || (weekday >= 0 && days[index].tm_wday == weekday);
      byNumber[index] = byNumber[index] || (number && days[index].tm_mday == number->value);
    }
    if (!byOrdinal.empty() && call_time::wordAt(heard, at, "primero"))
      byOrdinal.front() = true;
    if (!byOrdinal.empty() && call_time::anyWordAt(heard, at, {"primera", "first"}))
      byOrdinal.front() = true;
    if (byOrdinal.size() > 1 && call_time::anyWordAt(heard, at, {"segundo", "segunda", "second"}))
      byOrdinal[1] = true;
  }
  const bool saidWeekday = std::ranges::any_of(byWeekday, [](bool hit) { return hit; });
  if (saidWeekday)
    return pick(byWeekday);
  if (std::ranges::any_of(byRelative, [](bool hit) { return hit; }))
    return pick(byRelative);
  if (std::ranges::any_of(byNumber, [](bool hit) { return hit; }))
    return pick(byNumber);
  return pick(byOrdinal);
}

bool isDateTime(const argus::mcp::ToolSpec& spec, std::string_view field)
{
  const Json::Value& property = spec.inputSchema["properties"][std::string(field)];
  return property.isObject() && property["format"].isString() && property["format"].asString() == "date-time";
}

Filled fill(const FillInput& input)
{
  Filled filled{.arguments = input.arguments.isObject() ? input.arguments : Json::Value(Json::objectValue), .missing = {}};
  const auto present = [&filled](const std::string& field) {
    const Json::Value& value = filled.arguments[field];
    return !value.isNull() && !(value.isString() && value.asString().empty());
  };
  tools::ToolCall call;
  call.name = input.spec.name;
  call.arguments = filled.arguments;
  call.context = input.context;
  time_arguments::normalize({.call = call, .spec = input.spec, .now = input.now});
  for (const std::string& field : input.fields) {
    if (present(field))
      continue;
    if (isDateTime(input.spec, field)) {
      const Json::Value& value = call.arguments[field];
      if (value.isString() && iso_time::parse(value.asString())) {
        filled.arguments[field] = value;
        continue;
      }
      if (!filled.dispute && !filled.farField && !filled.passed) {
        const CallReading reading = dayReading({.utterance = input.context.utterance, .lang = input.context.lang, .now = input.now});
        if (reading.conflict)
          filled.dispute = Dispute{.field = field, .conflict = *reading.conflict};
        else if (reading.farAway)
          filled.farField = field;
        else if (reading.passedToday)
          filled.passed = Passed{.field = field, .tomorrowAt = *reading.passedToday};
      }
      filled.missing.push_back(field);
      continue;
    }
    if (field == "module") {
      const auto module = module_command::moduleNamedIn({.utterance = input.context.utterance, .modules = input.modules});
      if (module)
        filled.arguments[field] = *module;
      else
        filled.missing.push_back(field);
      continue;
    }
    const bool freeText = field == "title" || field == "name";
    const auto text = input.answering && freeText ? answerText(input.context.utterance)
                                                  : input.text.extract({.tool = input.spec.name,
                                                                        .field = field,
                                                                        .utterance = input.context.utterance,
                                                                        .lang = input.context.lang});
    if (text && !text->empty())
      filled.arguments[field] = *text;
    else
      filled.missing.push_back(field);
  }
  return filled;
}

}
