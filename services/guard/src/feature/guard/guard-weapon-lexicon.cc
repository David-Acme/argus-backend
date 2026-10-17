#include "guard-weapon-lexicon.hxx"

#include <text/text-norm.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct Token
{
  std::string word;
  std::size_t sentence{0};
};

constexpr std::array<std::string_view, 14> kWeapons{{
    "knife", "blade", "machete", "gun", "pistol", "rifle", "firearm",
    "cuchillo", "navaja", "arma", "pistola", "revolver", "escopeta", "fusil"}};

constexpr std::array<std::string_view, 14> kNegations{{
    "no", "not", "never", "without", "none", "neither", "nor",
    "sin", "ni", "ningun", "ninguna", "ninguno", "tampoco", "jamas"}};

constexpr std::array<std::string_view, 7> kNegationBarriers{{
    "but", "however", "although", "though", "yet", "pero", "aunque"}};

constexpr std::array<std::string_view, 4> kExamples{{
    "like", "such", "as", "como"}};

constexpr std::array<std::string_view, 10> kHedges{{
    "resemble", "resembles", "resembled", "resembling", "similar",
    "parece", "parecen", "parecia", "parecido", "parecida"}};

constexpr std::array<std::string_view, 22> kDeterminers{{
    "a", "an", "the", "un", "una", "uno", "el", "la", "los", "las", "su",
    "sus", "his", "her", "their", "its", "my", "our", "your", "other",
    "otro", "otra"}};

constexpr std::array<std::string_view, 47> kContactVerbs{{
    "hold", "holds", "holding", "held",
    "carry", "carries", "carrying", "carried",
    "grip", "grips", "gripping", "gripped",
    "grasp", "grasps", "grasping",
    "wield", "wields", "wielding",
    "brandish", "brandishes", "brandishing",
    "clutch", "clutches", "clutching",
    "cut", "cuts", "cutting",
    "sostiene", "sostener", "sosteniendo",
    "sujeta", "sujetando",
    "agarra", "agarrando",
    "empuna", "empunando",
    "lleva", "llevar", "llevaba", "llevando",
    "porta", "portando",
    "esgrime", "blande",
    "corta", "cortar", "cortando"}};

constexpr std::array<std::string_view, 5> kForwardVerbs{{
    "held", "carried", "gripped", "grasped", "clutched"}};

constexpr std::array<std::string_view, 2> kInstruments{{"with", "con"}};

constexpr std::array<std::string_view, 4> kHands{{
    "hand", "hands", "mano", "manos"}};

constexpr std::array<std::string_view, 2> kHandPrepositions{{"in", "en"}};

template <typename Range>
bool listed(const Range& words, std::string_view word)
{
  return std::ranges::find(words, word) != words.end();
}

std::vector<Token> tokenize(const std::string& caption)
{
  const std::string folded = text_norm::stripAccents(text_norm::nfc(caption));
  std::vector<Token> tokens;
  std::size_t sentence = 0;
  bool boundary = false;
  std::string current;
  for (const unsigned char byte : folded) {
    if (byte >= 0x80 || std::isalnum(byte) != 0) {
      if (boundary) {
        ++sentence;
        boundary = false;
      }
      current += static_cast<char>(std::tolower(byte));
      continue;
    }
    if (!current.empty()) {
      tokens.push_back({.word = current, .sentence = sentence});
      current.clear();
    }
    if (byte == '.' || byte == '!' || byte == '?' || byte == ';' ||
        byte == ':' || byte == '\n')
      boundary = true;
  }
  if (!current.empty())
    tokens.push_back({.word = current, .sentence = sentence});
  return tokens;
}

bool isWeaponWord(std::string_view word)
{
  if (word == "knives")
    return true;
  if (listed(kWeapons, word))
    return true;
  if (word.ends_with("es") && listed(kWeapons, word.substr(0, word.size() - 2)))
    return true;
  if (word.ends_with('s') && listed(kWeapons, word.substr(0, word.size() - 1)))
    return true;
  return false;
}

bool assertsWeaponAt(const std::vector<Token>& tokens, std::size_t index)
{
  const std::size_t sentence = tokens[index].sentence;
  const auto headBeforeDeterminers = [&tokens, sentence]<std::size_t Limit>(
                                        std::size_t from) {
    std::size_t at = from;
    std::size_t skipped = 0;
    while (at > 0 && skipped < Limit && tokens[at - 1].sentence == sentence &&
           listed(kDeterminers, tokens[at - 1].word)) {
      --at;
      ++skipped;
    }
    return at;
  };
  for (std::size_t at = index; at-- > 0;) {
    if (tokens[at].sentence != sentence)
      break;
    if (listed(kNegationBarriers, tokens[at].word))
      break;
    if (listed(kNegations, tokens[at].word))
      return false;
  }
  const std::size_t head = headBeforeDeterminers.operator()<2>(index);
  if (head > 0 && tokens[head - 1].sentence == sentence &&
      (listed(kExamples, tokens[head - 1].word) ||
       listed(kHedges, tokens[head - 1].word)))
    return false;
  if (index + 1 < tokens.size() && tokens[index + 1].sentence == sentence &&
      (tokens[index + 1].word == "like" || tokens[index + 1].word == "shaped"))
    return false;
  const std::size_t handEnd = std::min(tokens.size(), index + 6);
  for (std::size_t at = index + 1; at < handEnd; ++at) {
    if (tokens[at].sentence != sentence)
      break;
    if (!listed(kHandPrepositions, tokens[at].word))
      continue;
    for (std::size_t hand = at + 1; hand < handEnd; ++hand) {
      if (tokens[hand].sentence != sentence)
        break;
      if (listed(kHands, tokens[hand].word))
        return true;
    }
  }
  std::size_t seen = 0;
  for (std::size_t at = index; at-- > 0;) {
    if (tokens[at].sentence != sentence || seen >= 5)
      break;
    if (listed(kContactVerbs, tokens[at].word))
      return true;
    if (!listed(kDeterminers, tokens[at].word))
      ++seen;
  }
  const std::size_t instrumentHead =
      headBeforeDeterminers.operator()<1>(index);
  if (instrumentHead > 0 && tokens[instrumentHead - 1].sentence == sentence &&
      listed(kInstruments, tokens[instrumentHead - 1].word))
    return true;
  const std::size_t forwardEnd = std::min(tokens.size(), index + 3);
  for (std::size_t at = index + 1; at < forwardEnd; ++at) {
    if (tokens[at].sentence != sentence)
      break;
    if (listed(kForwardVerbs, tokens[at].word))
      return true;
  }
  return false;
}

}

bool guard_weapon::captionAssertsWeapon(const std::string& caption)
{
  const std::vector<Token> tokens = tokenize(caption);
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    if (isWeaponWord(tokens[index].word) && assertsWeaponAt(tokens, index))
      return true;
  }
  return false;
}
