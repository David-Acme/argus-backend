#include "reply-claims.hxx"

#include "reply-claim-lexicon.hxx"

#include <text/name-match.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace reply_claims
{

namespace
{
using Words = std::vector<std::string>;

constexpr std::size_t kNegationReach = 3;
constexpr std::string_view kInvertedQuestion = "\xC2\xBF";
constexpr std::array<std::string_view, 6> kLeadingFillers{"por", "oye", "argus", "hey", "ok", "porfa"};

struct Sentence
{
  Words words;
  bool question{false};
};

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

void closeSentence(std::vector<Sentence>& out, std::string& raw, bool question)
{
  if (question) {
    const std::size_t comma = raw.rfind(',');
    if (comma != std::string::npos) {
      Sentence head{.words = wordsOf(raw.substr(0, comma)), .question = false};
      if (!head.words.empty())
        out.push_back(std::move(head));
      raw.erase(0, comma + 1);
    }
  }
  Sentence sentence{.words = wordsOf(raw), .question = question};
  if (!sentence.words.empty())
    out.push_back(std::move(sentence));
  raw.clear();
}

std::vector<Sentence> sentencesOf(std::string_view text)
{
  std::vector<Sentence> out;
  std::string raw;
  bool openQuestion = false;
  for (std::size_t at = 0; at < text.size(); ++at) {
    if (text.compare(at, kInvertedQuestion.size(), kInvertedQuestion) == 0) {
      closeSentence(out, raw, openQuestion);
      openQuestion = true;
      at += kInvertedQuestion.size() - 1;
      continue;
    }
    const char c = text[at];
    if (c == '?') {
      closeSentence(out, raw, true);
      openQuestion = false;
    }
    else if (c == '.' || c == '!' || c == '\n' || c == ';') {
      closeSentence(out, raw, openQuestion);
      openQuestion = false;
    }
    else {
      raw += c;
    }
  }
  closeSentence(out, raw, openQuestion);
  return out;
}

bool matchesAt(const Words& words, std::size_t at, std::string_view phrase)
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

bool anyAt(const Words& words, std::size_t at, std::span<const std::string_view> phrases)
{
  return std::ranges::any_of(phrases, [&](std::string_view phrase) { return matchesAt(words, at, phrase); });
}

bool anyBefore(const Words& words, std::size_t at, std::span<const std::string_view> phrases)
{
  for (std::size_t index = 0; index < at; ++index)
    if (anyAt(words, index, phrases))
      return true;
  return false;
}

bool negated(const Words& words, std::size_t at, const Lexicon& lexicon)
{
  const std::size_t from = at > kNegationReach ? at - kNegationReach : 0;
  for (std::size_t index = from; index < at; ++index)
    if (anyAt(words, index, lexicon.negators))
      return true;
  return false;
}

bool subordinate(const Words& words, std::size_t at)
{
  return (at >= 1 && words[at - 1] == "que") || (at >= 2 && words[at - 2] == "que");
}

bool followedBy(const Words& words, std::size_t at, std::string_view next)
{
  return at + 1 < words.size() && words[at + 1] == next;
}

bool inDeterminers(const Lexicon& lexicon, const std::string& word)
{
  return std::ranges::find(lexicon.determiners, std::string_view(word)) != lexicon.determiners.end();
}

struct Probe
{
  const Words& words;
  std::size_t at;
  const Lexicon& lexicon;
};

bool performedAt(const Probe& probe)
{
  const bool one = std::ranges::any_of(probe.lexicon.performed, [&](std::string_view phrase) {
    return matchesAt(probe.words, probe.at, phrase) && phrase.find(' ') == std::string_view::npos &&
           !followedBy(probe.words, probe.at, "que");
  });
  const bool several = std::ranges::any_of(probe.lexicon.performed, [&](std::string_view phrase) {
    return phrase.find(' ') != std::string_view::npos && matchesAt(probe.words, probe.at, phrase);
  });
  if (!one && !several)
    return false;
  return !subordinate(probe.words, probe.at);
}

bool performativeAt(const Probe& probe)
{
  const bool verb = std::ranges::find(probe.lexicon.performative, std::string_view(probe.words[probe.at])) !=
                    probe.lexicon.performative.end();
  return verb && probe.at + 1 < probe.words.size() && inDeterminers(probe.lexicon, probe.words[probe.at + 1]);
}

bool describedAt(const Probe& probe)
{
  const std::size_t from = probe.at > 2 ? probe.at - 2 : 0;
  for (std::size_t index = from; index < probe.at; ++index)
    if (anyAt(probe.words, index, probe.lexicon.copulas))
      return true;
  return false;
}

bool stateChangeAt(const Probe& probe)
{
  return std::ranges::any_of(probe.lexicon.states, [&](std::string_view phrase) {
    if (!matchesAt(probe.words, probe.at, phrase))
      return false;
    return phrase.starts_with("ya ") || phrase.starts_with("quedo ") || !describedAt(probe);
  });
}

struct Mode
{
  bool asked{false};
  bool appOnly{false};
};

bool claimedIn(const Sentence& sentence, const Lexicon& lexicon, const Mode& mode)
{
  if (sentence.question)
    return false;
  const Words& words = sentence.words;
  for (std::size_t at = 0; at < words.size(); ++at) {
    const Probe probe{.words = words, .at = at, .lexicon = lexicon};
    const bool strong = (performedAt(probe) || performativeAt(probe)) && !(mode.appOnly && anyAt(words, at, lexicon.opening));
    const bool weak = mode.asked && !mode.appOnly && anyAt(words, at, lexicon.markers);
    const bool state = mode.asked && stateChangeAt(probe);
    if (!strong && !weak && !state)
      continue;
    if (negated(words, at, lexicon) || anyBefore(words, at, lexicon.hedges))
      continue;
    return true;
  }
  return false;
}

bool requestedIn(const Words& words, const Lexicon& lexicon)
{
  for (std::size_t at = 0; at < words.size(); ++at) {
    if (anyAt(words, at, lexicon.requests))
      return true;
    const bool leading = at == 0 || (at == 1 && std::ranges::find(kLeadingFillers, std::string_view(words[0])) != kLeadingFillers.end());
    if (leading && anyAt(words, at, lexicon.leadingRequests))
      return true;
  }
  return false;
}

bool endsSentence(char c)
{
  return c == '.' || c == '!' || c == '?' || c == '\n' || c == ';';
}
}

bool claimsDone(const Reply& reply)
{
  const std::vector<Sentence> sentences = sentencesOf(reply.text);
  return std::ranges::any_of(sentences, [&](const Sentence& sentence) {
    return std::ranges::any_of(lexicons(), [&](const Lexicon& lexicon) {
      return (reply.lang.empty() || lexicon.language == reply.lang) &&
             claimedIn(sentence, lexicon, {.asked = reply.asked, .appOnly = reply.appOnly});
    });
  });
}

bool asksForAction(std::string_view utterance)
{
  const Words words = wordsOf(std::string(utterance));
  return std::ranges::any_of(lexicons(), [&](const Lexicon& lexicon) { return requestedIn(words, lexicon); });
}

std::string withoutFalseClaims(Plain plain)
{
  if (!claimsDone({.text = plain.text, .asked = asksForAction(plain.utterance), .appOnly = false, .lang = plain.lang}))
    return std::move(plain.text);
  return honest(plain.lang);
}

std::string honest(std::string_view lang)
{
  return std::string(lexiconFor(lang).honest);
}

std::string nudge(std::string_view lang)
{
  return std::string(lexiconFor(lang).nudge);
}

ClaimGate::ClaimGate(GateInput input) : input_(std::move(input)) {}

TokenCallback ClaimGate::callback()
{
  return [this](const std::string& token, bool done) { accept(token, done); };
}

void ClaimGate::finish()
{
  if (finished_)
    return;
  finished_ = true;
  if (input_.sink)
    input_.sink("", true);
}

void ClaimGate::release(const std::string& sentence)
{
  if (cut_ || sentence.empty())
    return;
  const bool legitimate = input_.legitimate && input_.legitimate();
  if (legitimate || !claimsDone({.text = sentence, .asked = input_.asked, .appOnly = input_.appOnly, .lang = input_.lang})) {
    spoken_ += sentence;
    if (input_.sink)
      input_.sink(sentence, false);
    return;
  }
  cut_ = true;
  const bool separate = !spoken_.empty() && std::isspace(static_cast<unsigned char>(spoken_.back())) == 0;
  const std::string reply = (separate ? " " : "") + honest(input_.lang);
  spoken_ += reply;
  if (input_.sink)
    input_.sink(reply, false);
}

void ClaimGate::accept(const std::string& token, bool done)
{
  if (cut_ || finished_) {
    if (done)
      finish();
    return;
  }
  pending_ += token;
  std::size_t end = 0;
  while (!cut_ && end < pending_.size()) {
    if (!endsSentence(pending_[end])) {
      ++end;
      continue;
    }
    release(pending_.substr(0, end + 1));
    pending_.erase(0, end + 1);
    end = 0;
  }
  if (done) {
    if (!cut_)
      release(pending_);
    pending_.clear();
    finish();
  }
}

}
