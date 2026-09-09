#include "lexicon-extractor.hxx"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace
{

std::string stripPunct(const std::string& s)
{
  const auto trim = [](unsigned char c) {
    return std::ispunct(c) || std::isspace(c);
  };
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && trim(static_cast<unsigned char>(s[begin])))
    ++begin;
  while (end > begin && trim(static_cast<unsigned char>(s[end - 1])))
    --end;
  return s.substr(begin, end - begin);
}

std::vector<std::string> tokenize(const std::string& text)
{
  std::vector<std::string> out;
  std::string cur;
  for (const char c : text) {
    if (c == ' ') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur.push_back(c);
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

bool contains(const std::vector<std::string>& table, const std::string& s)
{
  return std::find(table.begin(), table.end(), s) != table.end();
}

struct Classified
{
  const text_match::Match* predicate = nullptr;
  std::vector<const text_match::Match*> before;
};

} // namespace

std::string LexiconExtractor::normalize(std::string_view text)
{
  return TemporalResolver::normalize(text);
}

LexiconExtractor::LexiconExtractor() = default;

void LexiconExtractor::build(const std::vector<extract::LexiconEntry>& entries)
{
  predicates_.clear();
  kinship_.clear();
  firstPerson_.clear();
  stopwords_.clear();

  std::vector<text_match::PatternRef> refs;
  refs.reserve(entries.size());

  const auto addPredicate = [&](const extract::LexiconEntry& entry) {
    const std::string norm = normalize(entry.surface);
    if (norm.empty())
      return;
    const size_t payloadId = predicates_.size();
    predicates_.push_back({.surface = norm, .canonical = entry.canonical});
    refs.push_back({.classId = text_match::PatternClass::Predicate,
                    .payloadId = static_cast<uint32_t>(payloadId),
                    .text = predicates_.back().surface});
  };

  const auto addWord = [&](const std::string& surface,
                           text_match::PatternClass cls,
                           std::vector<std::string>& table) {
    const std::string norm = normalize(surface);
    if (norm.empty())
      return;
    table.push_back(norm);
    refs.push_back({.classId = cls,
                    .payloadId = static_cast<uint32_t>(table.size() - 1),
                    .text = table.back()});
  };

  for (const auto& entry : entries) {
    switch (entry.kind) {
      case LexiconKind::Predicate:
        addPredicate(entry);
        break;
      case LexiconKind::Kinship:
        addWord(entry.surface, text_match::PatternClass::Kinship, kinship_);
        break;
      case LexiconKind::FirstPerson:
        addWord(entry.surface, text_match::PatternClass::FirstPerson,
                firstPerson_);
        break;
      case LexiconKind::Stopword:
        addWord(entry.surface, text_match::PatternClass::Stopword, stopwords_);
        break;
    }
  }

  automaton_ = text_match::PhraseAutomaton::build(refs);
}

void LexiconExtractor::rebuild(
    const std::vector<extract::LexiconEntry>& entries)
{
  build(entries);
}

bool LexiconExtractor::extract(const extract::ExtractInput& input,
                               std::vector<extract::ExtractedFact>& out) const
{
  const std::string text = normalize(input.clause);
  if (text.empty() || !automaton_)
    return false;

  text_match::MatchBuffer buffer;
  automaton_->match(text, buffer);

  Classified hits;
  for (const text_match::Match& m : buffer.items) {
    const auto& p = automaton_->pattern(m.patternIndex);
    if (p.classId != text_match::PatternClass::Predicate)
      continue;
    if (!hits.predicate || m.begin < hits.predicate->begin ||
        (m.begin == hits.predicate->begin && m.end > hits.predicate->end))
      hits.predicate = &m;
  }
  if (!hits.predicate)
    return false;
  for (const text_match::Match& m : buffer.items) {
    if (m.end <= hits.predicate->begin)
      hits.before.push_back(&m);
  }

  const auto& rule =
      predicates_[automaton_->pattern(hits.predicate->patternIndex).payloadId];
  const std::string subjectRegion = text.substr(0, hits.predicate->begin);
  const std::string valueRaw = stripPunct(text.substr(hits.predicate->end));

  const auto phraseFrom = [&](const std::vector<std::string>& tokens,
                              size_t start) {
    std::vector<std::string> kept;
    for (size_t i = start; i < tokens.size(); ++i) {
      const std::string& token = tokens[i];
      if (kept.empty() &&
          (contains(stopwords_, token) || contains(firstPerson_, token)))
        continue;
      kept.push_back(token);
    }
    while (!kept.empty() && contains(stopwords_, kept.back()))
      kept.pop_back();
    std::string out;
    for (const auto& token : kept) {
      if (!out.empty())
        out += " ";
      out += token;
    }
    return out;
  };

  const std::vector<std::string> subjectTokens = tokenize(subjectRegion);
  std::string subject;
  std::string value = valueRaw;

  size_t kinshipAt = subjectTokens.size();
  for (size_t i = 0; i < subjectTokens.size(); ++i) {
    if (contains(kinship_, subjectTokens[i])) {
      kinshipAt = i;
      break;
    }
  }
  if (kinshipAt < subjectTokens.size())
    subject = phraseFrom(subjectTokens, kinshipAt);
  if (subject.empty())
    subject = phraseFrom(subjectTokens, 0);
  if (subject.empty()) {
    for (const auto* m : hits.before) {
      const auto& p = automaton_->pattern(m->patternIndex);
      if (p.classId == text_match::PatternClass::FirstPerson) {
        subject = "usuario";
        break;
      }
    }
  }
  if (subject.empty()) {
    const bool predicateFirstPerson =
        rule.surface == "me" || rule.surface.find("me ") != std::string::npos ||
        rule.surface == "i" || rule.surface.rfind("i ", 0) == 0 ||
        rule.surface.rfind("my ", 0) == 0;
    if (predicateFirstPerson)
      subject = "usuario";
  }
  if (subject.empty()) {
    const auto valueTokens = tokenize(value);
    for (size_t i = 1; i + 1 < valueTokens.size() && i < 4; ++i) {
      const bool owner = valueTokens[i] == "mi" || valueTokens[i] == "my";
      if (!owner)
        continue;
      const std::string candidate = valueTokens[i + 1];
      if (contains(kinship_, candidate)) {
        subject = candidate;
        value.clear();
        for (size_t k = 0; k < i; ++k) {
          if (k > 0)
            value += " ";
          value += valueTokens[k];
        }
        break;
      }
    }
  }
  if (subject.empty())
    return false;

  extract::ExtractedFact fact;
  fact.subject = subject;
  fact.predicate = rule.canonical;
  fact.value = value.empty() ? valueRaw : value;
  fact.when =
      temporal_.resolve({.lang = input.lang, .normalized = fact.value});
  if (fact.when.kind != extract::TemporalKind::None)
    fact.factType = "schedule";
  else if (fact.predicate == "allergic_to")
    fact.factType = "attribute";
  else if (fact.predicate == "prefers" || fact.predicate == "likes" ||
           fact.predicate == "dislikes")
    fact.factType = "preference";
  else
    fact.factType = "attribute";
  fact.confidence = 0.9F;
  fact.tier = extract::ExtractTier::Lexicon;
  out.push_back(std::move(fact));
  return true;
}
