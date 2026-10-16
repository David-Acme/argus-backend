#include "turn-coherence.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace
{

constexpr std::string_view kAddressWord = "argus";

constexpr std::array<std::string_view, 47> kFillerWords{
    "a",   "al",   "de",  "del",  "el",   "la",   "las",  "lo",    "los",  "un",   "una",  "y",
    "o",   "e",    "que", "en",   "con",  "por",  "para", "pero",  "si",   "sí",   "no",   "me",
    "te",  "se",   "mi",  "tu",   "yo",   "es",   "eh",   "ah",    "oh",   "uh",   "um",   "pues",
    "ya",  "ok",   "vale", "the", "and",  "of",   "to",   "is",    "it",   "i",    "you"};

constexpr std::array<std::string_view, 9> kPunctuation{
    "\xC2\xBF", "\xC2\xA1", "\xC2\xAB", "\xC2\xBB", "\xE2\x80\x9C",
    "\xE2\x80\x9D", "\xE2\x80\xA6", "\xE2\x80\x94", "\xE2\x80\x93"};

std::size_t punctuationAt(std::string_view transcript, std::size_t at)
{
  for (const std::string_view mark : kPunctuation)
    if (transcript.substr(at, mark.size()) == mark)
      return mark.size();
  return 0;
}

std::vector<std::string> tokensOf(std::string_view transcript)
{
  std::vector<std::string> tokens;
  std::string current;
  for (std::size_t at = 0; at < transcript.size(); ++at) {
    const auto byte = static_cast<unsigned char>(transcript[at]);
    if (const std::size_t mark = punctuationAt(transcript, at); mark > 0) {
      if (!current.empty()) {
        tokens.push_back(std::move(current));
        current.clear();
      }
      at += mark - 1;
      continue;
    }
    if (byte >= 0x80 || std::isalnum(byte)) {
      current.push_back(byte >= 0x80 ? static_cast<char>(byte) : static_cast<char>(std::tolower(byte)));
      continue;
    }
    if (!current.empty()) {
      tokens.push_back(std::move(current));
      current.clear();
    }
  }
  if (!current.empty())
    tokens.push_back(std::move(current));
  return tokens;
}

bool isFiller(std::string_view token)
{
  return std::find(kFillerWords.begin(), kFillerWords.end(), token) != kFillerWords.end();
}

}

bool isAddressed(std::string_view transcript)
{
  const auto tokens = tokensOf(transcript);
  return std::find(tokens.begin(), tokens.end(), kAddressWord) != tokens.end();
}

bool hasContentWords(std::string_view transcript)
{
  constexpr size_t kMinContentBytes = 3;
  const auto tokens = tokensOf(transcript);
  return std::any_of(tokens.begin(), tokens.end(), [](const std::string& token) {
    return token.size() >= kMinContentBytes && token != kAddressWord && !isFiller(token);
  });
}

TurnCoherence turnCoherenceOf(std::string_view transcript)
{
  if (hasContentWords(transcript))
    return TurnCoherence::Proceed;
  if (isAddressed(transcript))
    return TurnCoherence::AskAddressed;
  return TurnCoherence::Ignore;
}
