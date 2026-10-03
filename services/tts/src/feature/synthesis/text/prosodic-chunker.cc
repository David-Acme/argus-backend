#include "prosodic-chunker.hxx"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>

namespace
{
constexpr std::string_view kEllipsis = "\xE2\x80\xA6";

constexpr std::array<std::string_view, 25> kAbbreviations{
    "sr", "sra", "srta", "dr",  "dra", "ud",  "uds", "etc",   "vs", "ej", "av", "pag", "num",
    "tel", "min", "max", "aprox", "mr", "mrs", "ms",  "prof", "st", "no", "vol", "fig"};

bool isSpace(char value)
{
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

bool isDigitChar(char value)
{
  return value >= '0' && value <= '9';
}

bool isAsciiAlpha(char value)
{
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool isCloser(char value)
{
  return value == '"' || value == '\'' || value == ')' || value == ']';
}

std::string_view trim(std::string_view text)
{
  while (!text.empty() && isSpace(text.front()))
    text.remove_prefix(1);
  while (!text.empty() && isSpace(text.back()))
    text.remove_suffix(1);
  return text;
}

std::string collapseSpaces(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  bool pending = false;
  for (const char value : text) {
    if (isSpace(value)) {
      pending = !out.empty();
      continue;
    }
    if (pending)
      out += ' ';
    pending = false;
    out += value;
  }
  return out;
}

bool isAbbreviationDot(std::string_view text, std::size_t dot)
{
  if (dot + 1 < text.size() && isDigitChar(text[dot + 1]))
    return true;
  if (dot > 0 && isDigitChar(text[dot - 1]))
    return true;
  auto begin = dot;
  while (begin > 0 && !isSpace(text[begin - 1]) && text[begin - 1] != '.')
    --begin;
  std::string word(text.substr(begin, dot - begin));
  std::ranges::transform(word, word.begin(), [](char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
  });
  if (word.size() == 1 && isAsciiAlpha(word.front()))
    return true;
  return std::ranges::find(kAbbreviations, word) != kAbbreviations.end();
}

std::size_t terminatorLength(std::string_view text, std::size_t position)
{
  const char value = text[position];
  if (value == '.' || value == '!' || value == '?')
    return 1;
  if (text.substr(position).starts_with(kEllipsis))
    return kEllipsis.size();
  return 0;
}

std::vector<std::string_view> splitSentences(std::string_view paragraph)
{
  std::vector<std::string_view> sentences;
  std::size_t start = 0;
  std::size_t position = 0;
  while (position < paragraph.size()) {
    const auto length = terminatorLength(paragraph, position);
    if (length == 0) {
      ++position;
      continue;
    }
    const bool dot = paragraph[position] == '.';
    const auto mark = position;
    auto after = position + length;
    while (after < paragraph.size()) {
      const auto more = terminatorLength(paragraph, after);
      if (more > 0)
        after += more;
      else if (isCloser(paragraph[after]))
        ++after;
      else
        break;
    }
    position = after;
    if (after < paragraph.size() && !isSpace(paragraph[after]))
      continue;
    if (dot && after == mark + 1 && isAbbreviationDot(paragraph, mark))
      continue;
    const auto sentence = trim(paragraph.substr(start, after - start));
    if (!sentence.empty())
      sentences.push_back(sentence);
    start = after;
  }
  const auto tail = trim(paragraph.substr(std::min(start, paragraph.size())));
  if (!tail.empty())
    sentences.push_back(tail);
  return sentences;
}

std::vector<std::string_view> splitAfter(std::string_view text, std::string_view delimiters)
{
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (std::size_t position = 0; position < text.size(); ++position) {
    if (delimiters.find(text[position]) == std::string_view::npos)
      continue;
    if (position + 1 < text.size() && !isSpace(text[position + 1]))
      continue;
    const auto part = trim(text.substr(start, position + 1 - start));
    if (!part.empty())
      parts.push_back(part);
    start = position + 1;
  }
  const auto tail = trim(text.substr(std::min(start, text.size())));
  if (!tail.empty())
    parts.push_back(tail);
  return parts;
}

std::vector<std::string_view> splitWords(std::string_view text)
{
  std::vector<std::string_view> words;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find(' ', start);
    const auto word = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (!word.empty())
      words.push_back(word);
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return words;
}

std::vector<std::string_view> splitDashes(std::string_view text)
{
  constexpr std::array<std::string_view, 3> kDashes{" \xE2\x80\x94 ", " \xE2\x80\x93 ", " - "};
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  std::size_t position = 0;
  while (position < text.size()) {
    const auto dash = std::ranges::find_if(kDashes, [&text, position](std::string_view candidate) {
      return text.substr(position).starts_with(candidate);
    });
    if (dash == kDashes.end()) {
      ++position;
      continue;
    }
    const auto part = trim(text.substr(start, position - start));
    if (!part.empty())
      parts.push_back(part);
    position += dash->size();
    start = position;
  }
  const auto tail = trim(text.substr(std::min(start, text.size())));
  if (!tail.empty())
    parts.push_back(tail);
  return parts;
}

class Chunker
{
public:
  explicit Chunker(const ProsodicChunkInput& input) : input_(input) {}

  std::vector<std::string> run()
  {
    std::vector<std::string> chunks;
    std::size_t start = 0;
    const auto text = input_.text;
    while (start <= text.size()) {
      auto end = text.find('\n', start);
      while (end != std::string_view::npos) {
        auto next = end + 1;
        while (next < text.size() && (text[next] == ' ' || text[next] == '\t' || text[next] == '\r'))
          ++next;
        if (next < text.size() && text[next] == '\n')
          break;
        end = text.find('\n', next);
      }
      const auto paragraph = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
      appendParagraph({.paragraph = paragraph, .chunks = chunks});
      if (end == std::string_view::npos)
        break;
      start = end + 1;
    }
    if (chunks.empty()) {
      const auto whole = collapseSpaces(text);
      if (!whole.empty())
        chunks.push_back(whole);
    }
    return chunks;
  }

private:
  struct ParagraphInput
  {
    std::string_view paragraph;
    std::vector<std::string>& chunks;
  };

  void appendParagraph(const ParagraphInput& input)
  {
    const auto paragraph = collapseSpaces(input.paragraph);
    if (paragraph.empty())
      return;
    std::vector<std::string> pieces;
    for (const auto sentence : splitSentences(paragraph)) {
      auto fitted = fit({.text = sentence, .level = 0});
      pieces.insert(pieces.end(), std::make_move_iterator(fitted.begin()), std::make_move_iterator(fitted.end()));
    }
    auto merged = merge(pieces);
    input.chunks.insert(input.chunks.end(), std::make_move_iterator(merged.begin()),
                        std::make_move_iterator(merged.end()));
  }

  [[nodiscard]] bool fits(std::string_view text) const
  {
    return input_.maxUnits == 0 || input_.measure(text) <= input_.maxUnits;
  }

  struct FitInput
  {
    std::string_view text;
    int level{0};
  };

  std::vector<std::string> fit(const FitInput& input)
  {
    if (fits(input.text))
      return {std::string(input.text)};
    std::vector<std::string_view> parts;
    switch (input.level) {
      case 0:
        parts = splitAfter(input.text, ";:");
        break;
      case 1:
        parts = splitAfter(input.text, ",");
        break;
      case 2:
        parts = splitDashes(input.text);
        break;
      case 3:
        parts = splitWords(input.text);
        break;
      default:
        return hardSplit(input.text);
    }
    if (parts.size() <= 1)
      return fit({.text = input.text, .level = input.level + 1});
    std::vector<std::string> pieces;
    for (const auto part : parts) {
      auto fitted = fit({.text = part, .level = input.level + 1});
      pieces.insert(pieces.end(), std::make_move_iterator(fitted.begin()), std::make_move_iterator(fitted.end()));
    }
    return merge(pieces);
  }

  std::vector<std::string> hardSplit(std::string_view text)
  {
    std::vector<std::string> pieces;
    std::size_t start = 0;
    while (start < text.size()) {
      std::size_t end = start;
      std::size_t best = start;
      while (end < text.size()) {
        auto next = end + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0U) == 0x80U)
          ++next;
        if (!fits(text.substr(start, next - start)))
          break;
        best = next;
        end = next;
      }
      if (best == start) {
        best = start + 1;
        while (best < text.size() && (static_cast<unsigned char>(text[best]) & 0xC0U) == 0x80U)
          ++best;
      }
      pieces.emplace_back(text.substr(start, best - start));
      start = best;
    }
    return pieces;
  }

  std::vector<std::string> merge(const std::vector<std::string>& pieces)
  {
    std::vector<std::string> merged;
    merged.reserve(pieces.size());
    std::string current;
    for (const auto& piece : pieces) {
      if (current.empty()) {
        current = piece;
        continue;
      }
      auto candidate = current;
      candidate += ' ';
      candidate += piece;
      if (fits(candidate)) {
        current = std::move(candidate);
        continue;
      }
      merged.push_back(std::move(current));
      current = piece;
    }
    if (!current.empty())
      merged.push_back(std::move(current));
    return merged;
  }

  const ProsodicChunkInput& input_;
};
}

std::vector<std::string> chunkProsodic(const ProsodicChunkInput& input)
{
  if (!input.measure && input.maxUnits > 0) {
    ProsodicChunkInput counted{.text = input.text, .maxUnits = input.maxUnits, .measure = codepointCount};
    return Chunker(counted).run();
  }
  return Chunker(input).run();
}

std::size_t codepointCount(std::string_view text)
{
  return static_cast<std::size_t>(std::ranges::count_if(text, [](char value) {
    return (static_cast<unsigned char>(value) & 0xC0U) != 0x80U;
  }));
}
