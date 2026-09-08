#include "text-norm.hxx"

#include <cctype>

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

std::string stripAccents(std::string text)
{
  const std::string src = "áéíóúüñÁÉÍÓÚÜÑ";
  const std::string dst = "aeiouunAEIOUUN";
  for (auto& c : text) {
    const size_t i = src.find(c);
    if (i != std::string::npos)
      c = dst[i];
  }
  return text;
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
