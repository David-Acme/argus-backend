#include "module-command.hxx"

#include "module-command-lexicon.hxx"

#include <text/name-match.hxx>

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <utility>

namespace module_command
{

namespace
{
using Words = std::vector<std::string>;

constexpr std::size_t kLongestAlias = 4;
constexpr std::array<std::string_view, 14> kAliasStopWords{
    "avisa", "importante", "tells", "matters", "compartidas", "shared", "familia", "family", "siempre", "vivo",
    "tiene", "cuenta", "muy", "pronto"};

Words wordsOf(std::string text)
{
  std::erase_if(text, [](char c) { return c == '\'' || c == '`'; });
  for (std::size_t at = text.find("\xE2\x80\x99"); at != std::string::npos; at = text.find("\xE2\x80\x99", at))
    text.erase(at, 3);
  const std::string folded = text_norm::folded(text);
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

bool phraseAt(const Words& words, std::size_t at, std::string_view phrase)
{
  std::size_t index = at;
  std::size_t begin = 0;
  while (begin <= phrase.size()) {
    const std::size_t end = std::min(phrase.find(' ', begin), phrase.size());
    if (index >= words.size() || words[index] != phrase.substr(begin, end - begin))
      return false;
    ++index;
    begin = end + 1;
  }
  return true;
}

bool anyPhraseAt(const Words& words, std::size_t at, Group group)
{
  for (const Table& table : tables()) {
    const auto& phrases = table.groups[static_cast<std::size_t>(group)];
    if (std::ranges::any_of(phrases, [&](std::string_view phrase) { return phraseAt(words, at, phrase); }))
      return true;
  }
  return false;
}

class Reading
{
public:
  explicit Reading(Words words) : words_(std::move(words))
  {
    start_ = 0;
    while (start_ < words_.size() && anyPhraseAt(words_, start_, Group::Filler))
      ++start_;
  }

  [[nodiscard]] const Words& words() const noexcept { return words_; }

  [[nodiscard]] bool has(Group group) const
  {
    for (std::size_t at = 0; at < words_.size(); ++at)
      if (anyPhraseAt(words_, at, group))
        return true;
    return false;
  }

  [[nodiscard]] bool opens(Group group) const { return start_ < words_.size() && anyPhraseAt(words_, start_, group); }

  [[nodiscard]] bool createdWithThat() const
  {
    for (std::size_t at = 0; at + 1 < words_.size(); ++at)
      if (anyPhraseAt(words_, at, Group::CreateVerb) && words_[at + 1] == "que")
        return true;
    return false;
  }

private:
  Words words_;
  std::size_t start_{0};
};

bool isAlias(const std::string& word)
{
  return word.size() > kLongestAlias && std::ranges::find(kAliasStopWords, std::string_view(word)) == kAliasStopWords.end();
}

std::set<std::string> vocabularyOf(const ModuleFlag& module)
{
  std::set<std::string> vocabulary;
  for (const std::string& text : {module.id, module.name.es, module.name.en, module.summary.es, module.summary.en})
    for (const std::string& word : wordsOf(text))
      if (isAlias(word))
        vocabulary.insert(word);
  return vocabulary;
}

bool inVocabulary(const std::set<std::string>& vocabulary, const std::string& word)
{
  if (vocabulary.contains(word))
    return true;
  if (vocabulary.contains(word + "s"))
    return true;
  return word.size() > kLongestAlias && word.back() == 's' && vocabulary.contains(word.substr(0, word.size() - 1));
}

std::optional<std::string> targetOf(const Words& words, const ModuleSnapshot& modules)
{
  std::map<std::string, int> votes;
  for (const ModuleFlag& module : modules.modules()) {
    if (module.id == kCoreModule || module.kind == "core")
      continue;
    const std::set<std::string> vocabulary = vocabularyOf(module);
    std::set<std::string> matched;
    for (const std::string& word : words)
      if (word.size() > kLongestAlias && inVocabulary(vocabulary, word))
        matched.insert(word);
    if (!matched.empty())
      votes[module.id] = static_cast<int>(matched.size());
  }
  if (votes.empty())
    return std::nullopt;
  const auto best = std::ranges::max_element(votes, {}, &std::pair<const std::string, int>::second);
  const auto tied = std::ranges::count_if(votes, [&](const auto& entry) { return entry.second == best->second; });
  if (tied != 1)
    return std::nullopt;
  return best->first;
}

Command named(std::string tool)
{
  return {.tool = std::move(tool), .arguments = Json::Value(Json::objectValue), .fill = {}};
}

struct OnModule
{
  std::string tool;
  const std::string& module;
};

Command forModule(OnModule target)
{
  Command command = named(std::move(target.tool));
  command.arguments["module"] = target.module;
  return command;
}

Command filled(std::string tool, std::vector<std::string> fields)
{
  Command command = named(std::move(tool));
  command.fill = std::move(fields);
  return command;
}

bool queryish(const Reading& reading)
{
  return reading.opens(Group::QueryMarker);
}

std::optional<Command> moduleRules(const Reading& reading, const std::optional<std::string>& module)
{
  if (module) {
    if (reading.has(Group::RequestVerb) && (reading.has(Group::OwnerNoun) || reading.has(Group::EnableVerb)))
      return forModule({.tool = "modules.request", .module = *module});
    if (reading.has(Group::PurgeVerb) && reading.has(Group::DataNoun))
      return forModule({.tool = "modules.open_purge_screen", .module = *module});
    if (reading.has(Group::DisableVerb))
      return forModule({.tool = "modules.disable", .module = *module});
    if (reading.has(Group::EnableVerb))
      return forModule({.tool = "modules.enable", .module = *module});
    if (reading.has(Group::ExplainMarker) || (reading.has(Group::ExplainGeneric) && reading.has(Group::ModuleNoun)))
      return forModule({.tool = "modules.explain", .module = *module});
  }
  if (reading.has(Group::ModulePlural) && queryish(reading))
    return named("modules.list");
  return std::nullopt;
}

bool otherFamily(const Reading& reading)
{
  return reading.has(Group::TaskNoun) || reading.has(Group::ProjectNoun) || reading.has(Group::ModuleNoun) ||
         reading.has(Group::ReminderNoun);
}

std::optional<Command> taskRules(const Reading& reading)
{
  if (reading.has(Group::ModuleNoun) || reading.has(Group::ReminderNoun))
    return std::nullopt;
  const bool markedDone = reading.has(Group::MarkVerb) && reading.has(Group::DoneMark);
  const bool completed = reading.has(Group::CompleteStrong) || (reading.has(Group::CompleteVerb) && reading.has(Group::TaskNoun));
  if (completed || markedDone)
    return filled("task.complete", {"title"});
  const bool taskCreate = (reading.has(Group::CreateVerb) && reading.has(Group::TaskNoun)) ||
                          (reading.has(Group::NewMark) && reading.has(Group::TaskNoun));
  if (taskCreate && !reading.createdWithThat())
    return filled("task.create", {"title"});
  if (reading.has(Group::TaskNoun) && queryish(reading))
    return named("task.list");
  return std::nullopt;
}

std::optional<Command> projectRules(const Reading& reading)
{
  if (!reading.has(Group::ProjectNoun) || reading.has(Group::ProjectVeto) || reading.has(Group::ScreenWord) || reading.has(Group::ModuleNoun) ||
      reading.has(Group::TaskNoun) || reading.has(Group::ReminderNoun))
    return std::nullopt;
  if ((reading.has(Group::ProjectVerb) || reading.has(Group::NewMark)) && !queryish(reading))
    return filled("project.create", {"name"});
  if (queryish(reading))
    return named("project.list");
  return std::nullopt;
}

std::optional<Command> calendarRules(const Reading& reading)
{
  if (otherFamily(reading) || reading.has(Group::BookingObject))
    return std::nullopt;
  const bool dated = reading.has(Group::EventNoun) || reading.has(Group::CalendarNoun);
  if (reading.has(Group::CancelVerb) && dated)
    return filled("calendar.cancel_event", {"title"});
  const bool plainCreate = reading.has(Group::CreateVerb) && dated && !reading.createdWithThat();
  const bool strongCreate = reading.has(Group::StrongCreate) && (dated || reading.has(Group::TimeMark));
  const bool leadingAgenda = reading.opens(Group::LeadAgenda);
  if ((plainCreate || strongCreate || leadingAgenda) && !queryish(reading))
    return filled("calendar.create_event", {"title", "starts_at"});
  if ((dated && queryish(reading)) || reading.has(Group::FreeMarker) || reading.has(Group::HaveScheduled))
    return named("calendar.list_events");
  return std::nullopt;
}
}

std::vector<std::string> tokensOf(std::string_view text)
{
  return wordsOf(std::string(text));
}

std::size_t phraseLength(Group group, const Words& words, std::size_t at)
{
  std::size_t longest = 0;
  for (const Table& table : tables())
    for (const std::string_view phrase : table.groups[static_cast<std::size_t>(group)])
      if (phraseAt(words, at, phrase))
        longest = std::max(longest, static_cast<std::size_t>(std::ranges::count(phrase, ' ') + 1));
  return longest;
}

bool declines(std::string_view utterance)
{
  return Reading(wordsOf(std::string(utterance))).opens(Group::Decline);
}

std::optional<std::string> moduleNamedIn(const Input& input)
{
  return targetOf(wordsOf(std::string(input.utterance)), input.modules);
}

std::optional<Command> commandFor(const Input& input)
{
  const Reading reading(wordsOf(std::string(input.utterance)));
  if (reading.words().empty())
    return std::nullopt;
  const std::optional<std::string> module = targetOf(reading.words(), input.modules);
  if (auto command = moduleRules(reading, module))
    return command;
  if (auto command = taskRules(reading))
    return command;
  if (auto command = projectRules(reading))
    return command;
  return calendarRules(reading);
}

}
