#include "pocket-prompt.hxx"

#include <array>
#include <span>

namespace
{
constexpr std::array<std::string_view, 8> kClosers{"\"", "'", "\xE2\x80\x9D", "\xE2\x80\x99", ")", "]", "\xC2\xBB", " "};
constexpr std::array<std::string_view, 4> kTerminal{".", "!", "?", "\xE2\x80\xA6"};
constexpr std::array<std::string_view, 7> kWeak{",", ";", ":", "-", "\xE2\x80\x93", "\xE2\x80\x94", " "};

bool isSpace(char value)
{
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v';
}

std::string_view trim(std::string_view text)
{
  while (!text.empty() && isSpace(text.front()))
    text.remove_prefix(1);
  while (!text.empty() && isSpace(text.back()))
    text.remove_suffix(1);
  return text;
}

std::size_t suffixLength(std::string_view text, std::span<const std::string_view> set)
{
  for (const auto candidate : set) {
    if (!candidate.empty() && text.ends_with(candidate))
      return candidate.size();
  }
  return 0;
}

std::string_view stripTrailing(std::string_view text, std::span<const std::string_view> set)
{
  while (const auto length = suffixLength(text, set))
    text.remove_suffix(length);
  return text;
}

std::string joinWords(std::string_view text)
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

struct Replacement
{
  std::string_view from;
  std::string_view to;
};

std::string replaceAll(std::string text, const Replacement& replacement)
{
  if (replacement.from.empty())
    return text;
  std::size_t position = 0;
  while ((position = text.find(replacement.from, position)) != std::string::npos) {
    text.replace(position, replacement.from.size(), replacement.to);
    position += replacement.to.size();
  }
  return text;
}

std::string dropPunctuationAfterSentenceEnd(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  std::size_t position = 0;
  while (position < text.size()) {
    const auto rest = text.substr(position);
    std::size_t mark = 0;
    for (const auto terminal : kTerminal) {
      if (rest.starts_with(terminal)) {
        mark = terminal.size();
        break;
      }
    }
    if (mark == 0) {
      out += text[position];
      ++position;
      continue;
    }
    out += rest.substr(0, mark);
    auto next = position + mark;
    while (next < text.size() && isSpace(text[next]))
      ++next;
    if (next < text.size() && (text[next] == ',' || text[next] == ';' || text[next] == ':'))
      position = next + 1;
    else
      position += mark;
  }
  return out;
}

std::size_t countWords(std::string_view text)
{
  std::size_t words = 0;
  bool inside = false;
  for (const char value : text) {
    if (isSpace(value))
      inside = false;
    else if (!inside) {
      inside = true;
      ++words;
    }
  }
  return words;
}

std::string capitalizeFirst(std::string text)
{
  if (text.empty())
    return text;
  const auto lead = static_cast<unsigned char>(text[0]);
  if (lead >= 'a' && lead <= 'z') {
    text[0] = static_cast<char>(lead - 'a' + 'A');
    return text;
  }
  if (lead == 0xC3U && text.size() > 1) {
    const auto trail = static_cast<unsigned char>(text[1]);
    if (trail >= 0xA0U && trail <= 0xBEU && trail != 0xB7U)
      text[1] = static_cast<char>(trail - 0x20U);
  }
  return text;
}

std::string ensureTerminalPunctuation(const std::string& text)
{
  const auto core = stripTrailing(text, kClosers);
  if (core.empty() || suffixLength(core, std::span(kTerminal).first(4)) > 0)
    return text;
  const auto closers = trim(std::string_view(text).substr(core.size()));
  if (suffixLength(core, std::span(kWeak).first(6)) > 0)
    return std::string(stripTrailing(core, kWeak)) + "." + std::string(closers);
  return text + ".";
}
}

PocketPrompt preparePocketPrompt(const PocketPromptInput& input)
{
  const auto& bundle = input.bundle;
  std::string text(trim(input.text));
  if (!bundle.replaceCharacters.empty()) {
    for (const auto& [from, to] : bundle.replaceCharacters)
      text = replaceAll(std::move(text), {.from = from, .to = to});
    text = dropPunctuationAfterSentenceEnd(joinWords(text));
  }
  if (text.empty())
    return {.text = {}, .framesAfterEos = 0};
  for (auto& value : text) {
    if (value == '\n' || value == '\r')
      value = ' ';
  }
  text = replaceAll(std::move(text), {.from = "  ", .to = " "});
  if (bundle.removeSemicolons)
    text = replaceAll(std::move(text), {.from = ";", .to = ","});
  const auto words = countWords(text);
  const int guess = words <= 4 ? 3 : 1;
  if (bundle.capitalizeFirstLetter)
    text = capitalizeFirst(std::move(text));
  if (bundle.appendTerminalPunctuation)
    text = ensureTerminalPunctuation(text);
  if (bundle.padWithSpaces && countWords(text) < 5)
    text.insert(0, 8, ' ');
  return {.text = std::move(text), .framesAfterEos = bundle.recommendedFramesAfterEos.value_or(guess + 2)};
}
