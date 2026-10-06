#include "name-match.hxx"

#include <text/text-norm.hxx>

#include <algorithm>
#include <cctype>

namespace text_norm
{

namespace
{
std::vector<std::string> wordsOf(const std::string& text)
{
  std::vector<std::string> words;
  const std::string spaced = folded(text);
  size_t at = 0;
  while (at < spaced.size()) {
    const size_t end = std::min(spaced.find(' ', at), spaced.size());
    if (end > at)
      words.push_back(spaced.substr(at, end - at));
    at = end + 1;
  }
  return words;
}

bool holdsAll(const std::vector<std::string>& name, const std::vector<std::string>& asked)
{
  return std::ranges::all_of(asked, [&name](const std::string& word) {
    return std::ranges::find(name, word) != name.end();
  });
}
}

std::string folded(const std::string& text)
{
  std::string out = stripAccents(text);
  for (auto& c : out)
    c = std::isalnum(static_cast<unsigned char>(c)) != 0 ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : ' ';
  out = whitespace(out);
  if (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

NameMatch matchName(const std::vector<std::string>& names, const std::string& asked)
{
  const auto words = wordsOf(asked);
  NameMatch match;
  if (words.empty())
    return match;
  const std::string spoken = folded(asked);
  for (size_t index = 0; index < names.size(); ++index) {
    if (folded(names[index]) == spoken) {
      match.kind = NameMatchKind::Exact;
      match.hits = {index};
      return match;
    }
  }
  for (size_t index = 0; index < names.size(); ++index)
    if (holdsAll(wordsOf(names[index]), words))
      match.hits.push_back(index);
  if (match.hits.size() == 1)
    match.kind = NameMatchKind::Exact;
  else if (match.hits.size() > 1)
    match.kind = NameMatchKind::Ambiguous;
  return match;
}

}
