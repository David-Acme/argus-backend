#include "guard-dialogue.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace
{

std::string lowercase(const std::string& value)
{
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value)
    result.push_back(static_cast<char>(std::tolower(character)));
  return result;
}

std::string stripAccents(const std::string& value)
{
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 15>
      kMap = {{{"á", "a"}, {"é", "e"}, {"í", "i"}, {"ó", "o"}, {"ú", "u"},
               {"ü", "u"}, {"à", "a"}, {"è", "e"}, {"ì", "i"}, {"ò", "o"},
               {"ù", "u"}, {"â", "a"}, {"ê", "e"}, {"î", "i"}, {"ô", "o"}}};
  std::string result;
  result.reserve(value.size());
  for (size_t index = 0; index < value.size();) {
    bool replaced = false;
    for (const auto& [from, to] : kMap) {
      if (value.compare(index, from.size(), from) == 0) {
        result.append(to);
        index += from.size();
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      result.push_back(value[index]);
      ++index;
    }
  }
  return result;
}

std::string normalize(const std::string& value)
{
  return stripAccents(lowercase(value));
}

std::vector<std::string> words(const std::string& value)
{
  std::vector<std::string> result;
  std::string current;
  for (const char character : value) {
    if (std::isspace(static_cast<unsigned char>(character))) {
      if (!current.empty()) {
        result.push_back(std::move(current));
        current.clear();
      }
      continue;
    }
    current.push_back(character);
  }
  if (!current.empty())
    result.push_back(std::move(current));
  return result;
}

bool containsAnyWord(const std::string& normalized,
                     const std::vector<std::string>& banned)
{
  for (const auto& token : banned) {
    if (normalized.find(token) != std::string::npos)
      return true;
  }
  return false;
}

const std::vector<std::string>& bannedVocabulary()
{
  static const std::vector<std::string> kBanned = {
      "camara",    "grabando",     "grabacion",    "grabar",
      "vigilancia", "vigilando",   "vigilar",      "monitoreo",
      "monitoreando", "peligro",   "alerta",       "sistema",
      "analisis",  "seguridad",    "alarma",       "sirena",
      "camera",    "recording",    "surveillance", "monitoring",
      "danger",    "alert",        "system",       "analysis",
      "security",  "alarm",        "siren",        "prompt",
      "instruccion", "instrucciones", "ignora",    "ignore",
      "jailbreak", "olvida"};
  return kBanned;
}

bool looksLikeDomain(std::string token)
{
  while (!token.empty() &&
         std::ispunct(static_cast<unsigned char>(token.front())) &&
         token.front() != '.')
    token.erase(token.begin());
  while (!token.empty() &&
         std::ispunct(static_cast<unsigned char>(token.back())) &&
         token.back() != '.')
    token.pop_back();
  if (token.empty())
    return false;
  if (token == "www" || token.rfind("www.", 0) == 0)
    return true;
  const auto dot = token.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= token.size())
    return false;
  const std::string tld = token.substr(token.rfind('.') + 1);
  if (tld.size() < 2 || tld.size() > 24)
    return false;
  return std::all_of(tld.begin(), tld.end(), [](const char character) {
    return std::isalpha(static_cast<unsigned char>(character)) != 0;
  });
}

bool hasUnsafeCharacters(const std::string& value)
{
  if (value.find("http") != std::string::npos ||
      value.find('@') != std::string::npos)
    return true;
  for (const char character : value) {
    if (std::isdigit(static_cast<unsigned char>(character)))
      return true;
  }
  for (const auto& token : words(value)) {
    if (looksLikeDomain(token))
      return true;
  }
  return false;
}

} // namespace

std::string guard_dialogue::sanitizeLine(const LineInput& input)
{
  const std::vector<std::string> tokens = words(input.text);
  if (tokens.empty())
    return {};
  if (static_cast<int>(tokens.size()) > std::max(1, input.maxWords))
    return {};

  const std::string normalized = normalize(input.text);
  if (containsAnyWord(normalized, bannedVocabulary()))
    return {};
  if (hasUnsafeCharacters(normalized))
    return {};

  if (std::count(input.text.begin(), input.text.end(), '?') > 1)
    return {};

  for (const auto& token : input.privateTokens) {
    if (token.size() < 3)
      continue;
    if (normalized.find(normalize(token)) != std::string::npos)
      return {};
  }

  std::string trimmed;
  trimmed.reserve(input.text.size());
  bool previousSpace = false;
  for (const char character : input.text) {
    const bool space = std::isspace(static_cast<unsigned char>(character));
    if (space) {
      if (!trimmed.empty() && !previousSpace)
        trimmed.push_back(' ');
      previousSpace = true;
      continue;
    }
    trimmed.push_back(character);
    previousSpace = false;
  }
  while (!trimmed.empty() && trimmed.back() == ' ')
    trimmed.pop_back();
  return trimmed;
}

std::string guard_dialogue::pickVaried(const VariantPickInput& input)
{
  if (input.variants.empty())
    return {};
  const size_t count = input.variants.size();
  const size_t start = static_cast<size_t>(
                           input.seed < 0 ? -input.seed : input.seed) %
                       count;
  for (size_t offset = 0; offset < count; ++offset) {
    const std::string& candidate = input.variants[(start + offset) % count];
    if (candidate != input.exclude)
      return candidate;
  }
  return input.variants[start];
}
