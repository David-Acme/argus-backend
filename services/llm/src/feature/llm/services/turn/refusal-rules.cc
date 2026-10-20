#include "refusal-rules.hxx"

#include <feature/llm/services/tools/whereabouts-command.hxx>
#include <text/name-match.hxx>
#include <text/text-norm.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace turn::refusal
{

namespace
{

constexpr std::size_t kMaxNameWords = 2;

struct Word
{
  std::string raw;
  std::string folded;
};

bool punctuationAt(std::string_view text, std::size_t at)
{
  if (at + 1 >= text.size() || static_cast<unsigned char>(text[at]) != 0xC2)
    return false;
  const auto next = static_cast<unsigned char>(text[at + 1]);
  return next == 0xA1 || next == 0xBF || next == 0xAB || next == 0xBB;
}

std::string lowerAscii(std::string text)
{
  std::ranges::transform(text, text.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return text;
}

std::vector<Word> wordsOf(std::string_view utterance)
{
  std::vector<Word> words;
  std::string raw;
  const auto close = [&] {
    if (!raw.empty())
      words.push_back({.raw = raw, .folded = lowerAscii(text_norm::stripAccents(raw))});
    raw.clear();
  };
  for (std::size_t at = 0; at < utterance.size(); ++at) {
    if (punctuationAt(utterance, at)) {
      close();
      ++at;
      continue;
    }
    const auto c = static_cast<unsigned char>(utterance[at]);
    if (std::isalnum(c) != 0 || c >= 0x80) {
      raw += utterance[at];
      continue;
    }
    close();
  }
  close();
  return words;
}

struct Hit
{
  std::size_t at{0};
  std::size_t size{0};
};

std::optional<Hit> firstHit(const std::vector<Word>& words, std::string_view phrase)
{
  const std::vector<Word> needle = wordsOf(phrase);
  if (needle.empty() || needle.size() > words.size())
    return std::nullopt;
  for (std::size_t at = 0; at + needle.size() <= words.size(); ++at) {
    bool hit = true;
    for (std::size_t index = 0; index < needle.size() && hit; ++index)
      hit = words.at(at + index).folded == needle.at(index).folded;
    if (hit)
      return Hit{.at = at, .size = needle.size()};
  }
  return std::nullopt;
}

std::optional<Hit> firstPhrase(const std::vector<Word>& words, std::span<const std::string_view> phrases)
{
  for (const std::string_view phrase : phrases)
    if (const auto hit = firstHit(words, phrase))
      return hit;
  return std::nullopt;
}

bool anyStem(const std::vector<Word>& words, std::span<const std::string_view> stems)
{
  return std::ranges::any_of(stems, [&words](std::string_view stem) {
    if (stem.find(' ') != std::string_view::npos)
      return firstHit(words, stem).has_value();
    return std::ranges::any_of(words, [stem](const Word& word) { return word.folded.starts_with(stem); });
  });
}

bool inList(std::string_view value, std::span<const std::string_view> list)
{
  return std::ranges::find(list, value) != list.end();
}

constexpr std::array<std::string_view, 13> kRelayStemsEs{
    "dile", "diles", "avisale", "avisales", "avisarle", "avisarles", "mandale", "mandales", "cuentale",
    "cuentales", "pasale", "pasarle", "pidele"};
constexpr std::array<std::string_view, 6> kRelayWordsEn{"tell", "let", "text", "forward", "relay", "warn"};
constexpr std::array<std::string_view, 3> kRelayAsksEs{"le avisas", "les avisas", "le avisaras"};

constexpr std::array<std::string_view, 22> kRelayStop{
    "que", "quien", "quienes", "como", "cuando", "donde", "i", "ill", "im", "ive", "that", "know", "to", "about",
    "if", "and", "y", "o", "no", "si", "ya", "otro"};
constexpr std::array<std::string_view, 10> kRelaySkip{
    "a", "al", "el", "la", "los", "las", "the", "to", "on", "de"};
constexpr std::array<std::string_view, 12> kRelayPronoun{
    "me", "him", "her", "them", "us", "you", "himself", "herself", "lo", "le", "les", "ella"};

constexpr std::array<std::string_view, 14> kImpersonationEs{
    "de parte de", "en nombre de", "hazte pasar", "haciendote pasar", "finge que eres", "finge ser",
    "como si fuera", "como si fueras", "que no sepa que soy", "sin decir que soy", "sin decirle que soy",
    "que no diga quien", "anonimo", "anonimamente"};
constexpr std::array<std::string_view, 12> kImpersonationEn{
    "on behalf of", "pretend to be", "pretend you are", "pretending to be", "as if it were", "as if you were",
    "pose as", "posing as", "say it is from", "anonymously", "without saying it is me", "do not say it is me"};

bool impersonates(const std::vector<Word>& words, bool english)
{
  return firstPhrase(words, english ? std::span<const std::string_view>(kImpersonationEn)
                                    : std::span<const std::string_view>(kImpersonationEs))
      .has_value();
}

constexpr std::array<std::string_view, 8> kWherePersonEs{
    "donde esta", "donde andara", "donde anda", "en que lugar esta", "donde vive", "donde estuvo", "donde se fue",
    "donde trabajo"};
constexpr std::array<std::string_view, 4> kWherePersonEn{"where is", "where was", "where did", "where are"};
constexpr std::array<std::string_view, 6> kPresenceEs{
    "quien esta en casa", "quienes estan en casa", "quien esta aqui", "quienes estan aqui", "hay alguien en casa",
    "quien anda por casa"};
constexpr std::array<std::string_view, 6> kPresenceEn{
    "who is home", "who's home", "who is at home", "who is here", "is anyone home", "anyone at home"};
constexpr std::array<std::string_view, 11> kOtherPrivateEs{
    "que recordatorios tiene", "que recordatorios tenia", "que agenda tiene", "que tareas tiene", "con quien hablo",
    "con quien estaba", "con quien se fue", "a quien llamo", "a quien le escribio", "que hizo hoy", "en que anda"};
constexpr std::array<std::string_view, 9> kOtherPrivateEn{
    "what reminders does", "what reminders did", "what agenda does", "what tasks does", "who did he talk",
    "who did she talk", "who was he with", "who was she with", "who did he call"};

constexpr std::array<std::string_view, 8> kSecurityEs{
    "novedad", "seguridad", "vigilancia", "alarma", "camara", "intruso", "incidente", "grabacion"};
constexpr std::array<std::string_view, 8> kSecurityEn{
    "news", "security", "surveillance", "alarm", "camera", "intruder", "incident", "footage"};

constexpr std::array<std::string_view, 7> kPostponeEs{
    "pospon", "aplaz", "para mas tarde", "para otro dia", "para despues", "pasalo para", "muevelo para"};
constexpr std::array<std::string_view, 7> kPostponeEn{
    "postpone", "push", "defer", "reschedule", "for later", "another time", "move it"};

inline constexpr std::string_view kPrivacySlot = "privacy";
inline constexpr std::string_view kPrivacyTool = "app.set_camera_privacy";
constexpr std::array<std::string_view, 2> kPrivacyStems{"privacidad", "privacy"};
constexpr std::array<std::string_view, 10> kPrivacyVerbEs{
    "activa", "activo", "apaga", "quita", "quitale", "pon", "ponle", "enciende", "desactiva", "configura"};
constexpr std::array<std::string_view, 9> kPrivacyVerbEn{
    "on", "off", "enable", "disable", "turn", "remove", "activate", "set", "switch"};

constexpr std::array<std::string_view, 6> kMoveVerbsEs{"pas", "muev", "mov", "dej", "cambi", "corr"};
constexpr std::array<std::string_view, 5> kMoveVerbsEn{"move", "push", "leave", "shift", "put"};

bool postponeAsked(const std::vector<Word>& words, bool english)
{
  const std::span<const std::string_view> phrases =
      english ? std::span<const std::string_view>(kPostponeEn) : std::span<const std::string_view>(kPostponeEs);
  const std::span<const std::string_view> verbs =
      english ? std::span<const std::string_view>(kMoveVerbsEn) : std::span<const std::string_view>(kMoveVerbsEs);
  return std::ranges::any_of(phrases, [&](std::string_view phrase) {
    if (phrase.find(' ') == std::string_view::npos)
      return std::ranges::any_of(words, [phrase](const Word& word) { return word.folded.starts_with(phrase); });
    return firstHit(words, phrase).has_value() && anyStem(words, verbs);
  });
}

struct NameWindow
{
  std::size_t start{0};
  std::size_t most{0};
};

std::optional<std::string> nameAt(const std::vector<Word>& words, const NameWindow& window)
{
  const std::size_t most = window.most;
  std::size_t index = window.start;
  while (index < words.size() && inList(words.at(index).folded, kRelaySkip))
    ++index;
  if (index >= words.size())
    return std::string();
  if (words.at(index).folded == "me")
    return std::nullopt;
  if (inList(words.at(index).folded, kRelayPronoun))
    return std::string();
  std::size_t collected = 0;
  const std::size_t begin = index;
  while (index < words.size() && collected < most) {
    const std::string& folded = words.at(index).folded;
    if (inList(folded, kRelayStop) || (collected > 0 && inList(folded, kRelaySkip)))
      break;
    if (inList(folded, kRelayPronoun))
      break;
    ++collected;
    ++index;
  }
  std::string name;
  for (std::size_t at = begin; at < begin + collected && at < words.size(); ++at) {
    if (!name.empty())
      name += ' ';
    name += words.at(at).raw;
  }
  return name;
}

bool namesCamera(const std::vector<Word>& words, std::size_t from)
{
  std::size_t index = from;
  while (index < words.size() && inList(words.at(index).folded, kRelaySkip))
    ++index;
  return index < words.size() &&
         (words.at(index).folded.starts_with("camara") || words.at(index).folded.starts_with("camera"));
}

std::string titled(std::string name)
{
  bool start = true;
  for (char& c : name) {
    if (start)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    start = c == ' ';
  }
  return name;
}

std::optional<std::string> relayTarget(const std::vector<Word>& words, bool english)
{
  if (english) {
    for (std::size_t at = 0; at + 1 < words.size(); ++at) {
      if (!inList(words.at(at).folded, kRelayWordsEn))
        continue;
      const std::size_t start = words.at(at + 1).folded == "on" ? at + 2 : at + 1;
      if (const auto name = nameAt(words, {.start = start, .most = kMaxNameWords}))
        return name;
    }
  }
  else {
    for (const std::string_view stem : kRelayStemsEs)
      for (std::size_t at = 0; at < words.size(); ++at)
        if (words.at(at).folded.starts_with(stem))
          return nameAt(words, {.start = at + 1, .most = kMaxNameWords});
    if (const auto hit = firstPhrase(words, kRelayAsksEs))
      return nameAt(words, {.start = hit->at + hit->size, .most = 0});
  }
  return std::nullopt;
}

}

std::optional<PolarityAsk> polarityAskFor(const Ask& ask)
{
  if (ask.utterance.empty())
    return std::nullopt;
  const bool english = ask.lang == "en";
  const std::vector<Word> words = wordsOf(ask.utterance);
  const bool target = std::ranges::any_of(kPrivacyStems, [&words](std::string_view stem) {
    return std::ranges::any_of(words, [stem](const Word& word) { return word.folded.starts_with(stem); });
  });
  if (!target)
    return std::nullopt;
  const bool asked = anyStem(words, english ? std::span<const std::string_view>(kPrivacyVerbEn)
                                            : std::span<const std::string_view>(kPrivacyVerbEs));
  if (asked)
    return std::nullopt;
  return PolarityAsk{.slot = std::string(kPrivacySlot), .tool = std::string(kPrivacyTool)};
}

std::optional<Refusal> refusalFor(const Ask& ask)
{
  if (ask.utterance.empty())
    return std::nullopt;
  const bool english = ask.lang == "en";
  const std::vector<Word> words = wordsOf(ask.utterance);
  const ModuleSnapshot& modules = ask.audience.modules;
  const bool owner = ask.audience.role == UserRole::Owner;

  if (const auto target = relayTarget(words, english)) {
    if (impersonates(words, english))
      return Refusal{.reason = std::string(kImpersonation), .name = *target};
    if (!ask.relayOffered)
      return Refusal{.reason = std::string(kNoRelay), .name = *target};
    return std::nullopt;
  }

  const bool security = anyStem(words, english ? std::span<const std::string_view>(kSecurityEn)
                                               : std::span<const std::string_view>(kSecurityEs));
  if (security && !modules.enabled("surveillance"))
    return Refusal{.reason = std::string(kModuleOff), .name = {}};

  if (firstPhrase(words, english ? std::span<const std::string_view>(kPresenceEn)
                                 : std::span<const std::string_view>(kPresenceEs))) {
    if (!owner)
      return Refusal{.reason = std::string(kPrivatePerson), .name = {}};
    return std::nullopt;
  }

  if (const auto hit = firstPhrase(words, english ? std::span<const std::string_view>(kWherePersonEn)
                                                  : std::span<const std::string_view>(kWherePersonEs))) {
    if (owner)
      return std::nullopt;
    if (!namesCamera(words, hit->at + hit->size))
      return Refusal{.reason = std::string(kPrivatePerson),
                     .name = nameAt(words, {.start = hit->at + hit->size, .most = 1}).value_or(std::string())};
  }

  const std::optional<tools::ToolCall> whereabouts =
      owner ? std::nullopt : whereaboutsCommandFor(std::string(ask.utterance));
  if (whereabouts)
    return Refusal{.reason = std::string(kPrivatePerson),
                   .name = titled(whereabouts->arguments.get("person", "").asString())};

  if (firstPhrase(words, english ? std::span<const std::string_view>(kOtherPrivateEn)
                                 : std::span<const std::string_view>(kOtherPrivateEs)))
    return Refusal{.reason = std::string(kPrivatePerson), .name = {}};

  if (postponeAsked(words, english))
    return Refusal{.reason = std::string(kNoAction), .name = {}};

  return std::nullopt;
}

}
