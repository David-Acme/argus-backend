#include "tiered-extractor.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string_view>
#include <shared/services/config-service/config-service.hxx>
#include <utility>

namespace
{

std::string foldLower(std::string_view s)
{
  std::string out = TemporalResolver::normalize(s);
  return out;
}

constexpr size_t kMaxComplementWords = 6;

size_t wordCount(const std::string& s)
{
  size_t n = 0;
  bool inWord = false;
  for (const char c : s) {
    const bool space = c == ' ' || c == '\t' || c == '\n';
    if (space) {
      inWord = false;
      continue;
    }
    if (!inWord) {
      inWord = true;
      ++n;
    }
  }
  return n;
}

struct ActionRepairInput
{
  std::string source;
  std::string subject;
  std::string object;
  std::string time;
};

std::string repairAction(const ActionRepairInput& in)
{
  size_t begin = 0;
  if (!in.subject.empty()) {
    const size_t at = in.source.find(in.subject);
    if (at != std::string::npos)
      begin = at + in.subject.size();
  }
  size_t end = in.source.size();
  for (const std::string& tail : {in.object, in.time}) {
    if (tail.empty())
      continue;
    const size_t at = in.source.find(tail, begin);
    if (at != std::string::npos && at < end)
      end = at;
  }
  if (end <= begin)
    return {};
  std::string span = in.source.substr(begin, end - begin);
  const size_t first = span.find_first_not_of(" \t");
  if (first == std::string::npos)
    return {};
  const size_t last = span.find_last_not_of(" \t");
  return span.substr(first, last - first + 1);
}

bool covers(std::string_view clause, const extract::ExtractedFact& fact)
{
  const auto words = [](std::string_view text) {
    size_t count = 0;
    bool inWord = false;
    for (const char c : text) {
      const bool space = c == ' ' || c == '\t' || c == '\n';
      if (space) {
        inWord = false;
        continue;
      }
      if (!inWord) {
        inWord = true;
        ++count;
      }
    }
    return count;
  };

  const size_t total = words(clause);
  if (total == 0)
    return true;
  const size_t taken = words(fact.subject) + words(fact.predicate) +
                       words(fact.value) + words(fact.when.surface);
  static const double floor = [] {
    const double ratio =
        ConfigService::getDouble("extract.lexicon_min_coverage");
    return ratio > 0.0 ? ratio : 0.6;
  }();
  return static_cast<double>(taken) / static_cast<double>(total) >= floor;
}

std::string firstWord(const std::string& text)
{
  const size_t at = text.find(' ');
  return at == std::string::npos ? text : text.substr(0, at);
}

std::string trimSubject(std::string subject, const std::string& predicate,
                        const std::string& value, const std::string& when,
                        std::string_view clause)
{
  const auto findWord = [](const std::string& hay, const std::string& needle) {
    if (needle.empty())
      return std::string::npos;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + 1)) {
      const bool leftOk = at == 0 || hay[at - 1] == ' ';
      const size_t end = at + needle.size();
      const bool rightOk = end >= hay.size() || hay[end] == ' ';
      if (leftOk && rightOk)
        return at;
    }
    return std::string::npos;
  };
  const auto cutAt = [&](const std::string& tail) {
    if (tail.empty())
      return;
    const std::string lower = foldLower(subject);
    size_t at = findWord(lower, foldLower(tail));
    if (at == std::string::npos)
      at = findWord(lower, foldLower(firstWord(tail)));
    if (at != std::string::npos && at > 0)
      subject.erase(at);
  };
  cutAt(predicate);
  cutAt(value);
  cutAt(when);

  static constexpr std::array<std::string_view, 22> kLeading{
      "el ", "la ", "los ", "las ", "un ", "una ", "mi ", "mis ",
      "tu ", "tus ", "su ", "sus ", "a ", "al ", "de ", "the ",
      "my ", "your ", "his ", "her ", "their ", "an "};
  for (;;) {
    const std::string lower = foldLower(subject);
    bool stripped = false;
    for (const std::string_view prefix : kLeading) {
      if (lower.rfind(prefix, 0) != 0)
        continue;
      subject.erase(0, prefix.size());
      stripped = true;
      break;
    }
    if (!stripped)
      break;
  }
  while (!subject.empty() && (subject.back() == ' ' || subject.back() == ','))
    subject.pop_back();

  if (subject.empty())
    return subject;

  const std::string source = foldLower(std::string(clause));
  const std::string needle = foldLower(subject);
  const size_t at = source.find(needle);
  if (at == std::string::npos)
    return subject;

  for (const char* link : {" de ", " del "}) {
    const size_t linkLen = std::strlen(link);
    const size_t after = at + needle.size();
    if (source.compare(after, linkLen, link) == 0) {
      const size_t tail = source.find(' ', after + linkLen);
      const size_t end = tail == std::string::npos ? source.size() : tail;
      return std::string(clause).substr(at, end - at);
    }
    if (at >= linkLen && source.compare(at - linkLen, linkLen, link) == 0) {
      const size_t head = source.rfind(' ', at - linkLen - 1);
      const size_t begin = head == std::string::npos ? 0 : head + 1;
      return std::string(clause).substr(begin, at + needle.size() - begin);
    }
  }
  return subject;
}

} // namespace

TieredExtractor::TieredExtractor(ExtractionService& model) : model_(model) {}

void TieredExtractor::rebuild(const std::vector<extract::LexiconEntry>& entries)
{
  lexicon_.rebuild(entries);
}

bool TieredExtractor::extract(const extract::ExtractInput& input,
                              std::vector<extract::ExtractedFact>& out) const
{
  if (!input.requireModel && lexicon_.extract(input, out)) {
    if (covers(input.clause, out.front()))
      return true;
    out.clear();
  }

  if (!input.allowModel)
    return false;

  if (!model_.isLoaded() && !model_.ensureLoaded())
    return false;

  const auto root =
      model_.extract({.text = std::string(input.clause),
                      .templateJson = extract::factTemplateFor(input.lang),
                      .schemaJson = extract::factSchemaFor(input.lang),
                      .maxTokens = 512,
                      .cancel = CancellationToken{}});
  if (!root)
    return false;
  addModelFacts(input, *root, out);
  return !out.empty();
}

void TieredExtractor::addModelFacts(
    const extract::ExtractInput& input, const Json::Value& root,
    std::vector<extract::ExtractedFact>& out) const
{
  Json::Value facts(Json::arrayValue);
  if (root["facts"].isArray())
    facts = root["facts"];
  else if (root.isObject() &&
           (root.isMember("entidad") || root.isMember("entity") ||
            root.isMember("subject")))
    facts.append(root);
  if (facts.empty())
    return;
  const std::string source = foldLower(input.clause);
  for (const Json::Value& f : facts) {
    std::string subject =
        f.get("entidad", f.get("entity", f.get("subject", ""))).asString();
    std::string predicate =
        f.get("verbo", f.get("verb", f.get("action", ""))).asString();
    std::string value =
        f.get("complemento", f.get("complement", f.get("object", "")))
            .asString();
    std::string when =
        f.get("cuando", f.get("when", f.get("time", ""))).asString();

    subject = trimSubject(subject, predicate, value, when, input.clause);

    if (!value.empty() && foldLower(value) == foldLower(when))
      value.clear();

    if (!predicate.empty() &&
        source.find(foldLower(predicate)) == std::string::npos)
      predicate = repairAction({.source = source,
                                .subject = foldLower(subject),
                                .object = foldLower(value),
                                .time = foldLower(when)});

    if (!value.empty() && source.find(foldLower(value)) == std::string::npos)
      value.clear();
    if (!when.empty() && source.find(foldLower(when)) == std::string::npos)
      when.clear();

    if (wordCount(value) > kMaxComplementWords)
      value.clear();
    if (wordCount(when) > kMaxComplementWords)
      when.clear();

    extract::TemporalValue resolved;
    temporal_.resolve(input.lang, foldLower(when), resolved);
    if (!when.empty() && resolved.kind == extract::TemporalKind::None) {
      when.clear();
      resolved = extract::TemporalValue{};
    }

    if (subject.empty() || predicate.empty())
      continue;
    if (source.find(foldLower(subject)) == std::string::npos)
      continue;

    extract::ExtractedFact fact;
    fact.subject = foldLower(subject);
    fact.predicate = foldLower(predicate);
    fact.value = foldLower(value);
    fact.when = resolved;
    fact.confidence = 0.75F;
    fact.tier = extract::ExtractTier::Model;
    out.push_back(std::move(fact));
  }
}
