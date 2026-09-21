#include "text-norm.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>

namespace text_norm
{

std::vector<std::string> words(const std::string& text, int minAlnum)
{
  std::vector<std::string> out;
  std::string current;
  int alnum = 0;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c >= 0x80) {
      current += static_cast<char>(std::tolower(c));
      if (std::isalnum(c))
        ++alnum;
    }
    else if (!current.empty()) {
      if (alnum >= minAlnum)
        out.push_back(current);
      current.clear();
      alnum = 0;
    }
  }
  if (alnum >= minAlnum)
    out.push_back(current);
  return out;
}

std::unordered_set<std::string> wordSet(const std::string& text, int minAlnum)
{
  std::unordered_set<std::string> out;
  std::string current;
  int alnum = 0;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c >= 0x80) {
      current += static_cast<char>(std::tolower(c));
      if (std::isalnum(c))
        ++alnum;
    }
    else if (!current.empty()) {
      if (alnum >= minAlnum)
        out.insert(current);
      current.clear();
      alnum = 0;
    }
  }
  if (alnum >= minAlnum)
    out.insert(current);
  return out;
}

std::string whitespace(const std::string& text, bool toLower)
{
  std::string out;
  bool lastSpace = true;
  for (unsigned char c : text) {
    if (std::isspace(c)) {
      if (!lastSpace)
        out += ' ';
      lastSpace = true;
    }
    else {
      out +=
          toLower ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
      lastSpace = false;
    }
  }
  return out;
}

// Multi-byte aware: a byte-wise pass would split the two-byte fold sequences.
std::string stripAccents(std::string text)
{
  static constexpr std::array<std::pair<std::string_view, char>, 35> kFolds{{
      {"á", 'a'}, {"é", 'e'}, {"í", 'i'}, {"ó", 'o'}, {"ú", 'u'},
      {"ü", 'u'}, {"ñ", 'n'}, {"à", 'a'}, {"è", 'e'}, {"ì", 'i'},
      {"ò", 'o'}, {"ù", 'u'}, {"â", 'a'}, {"ê", 'e'}, {"î", 'i'},
      {"ô", 'o'}, {"û", 'u'}, {"ä", 'a'}, {"ë", 'e'}, {"ï", 'i'},
      {"ö", 'o'}, {"ÿ", 'y'}, {"ç", 'c'}, {"Á", 'a'}, {"É", 'e'},
      {"Í", 'i'}, {"Ó", 'o'}, {"Ú", 'u'}, {"Ü", 'u'}, {"Ñ", 'n'},
      {"À", 'a'}, {"È", 'e'}, {"Ì", 'i'}, {"Ò", 'o'}, {"Ù", 'u'},
  }};
  std::string out;
  out.reserve(text.size());
  const std::string_view view(text);
  for (size_t i = 0; i < view.size();) {
    const auto fold = std::ranges::find_if(kFolds, [view, i](const auto& entry) {
      return view.substr(i).starts_with(entry.first);
    });
    if (fold == kFolds.end()) {
      out.push_back(view[i]);
      ++i;
      continue;
    }
    out.push_back(fold->second);
    i += fold->first.size();
  }
  return out;
}

std::string intent(const std::string& text)
{
  std::string out;
  out.reserve(text.size() + 1);
  bool lastSpace = true;
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == 0xC2 && i + 1 < n) {
      const unsigned char next = static_cast<unsigned char>(text[i + 1]);
      if (next == 0xA1 || next == 0xBF) {
        i += 2;
        continue;
      }
    }
    const bool punct =
        c == '?' || c == '!' || c == '.' || c == ',' || c == ';' || c == ':';
    const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
    if (punct) {
      if (!lastSpace)
        out += ' ';
      lastSpace = true;
      ++i;
      continue;
    }
    if (space) {
      if (!lastSpace)
        out += ' ';
      lastSpace = true;
      ++i;
      continue;
    }
    out += text[i];
    lastSpace = false;
    ++i;
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

} // namespace text_norm
