#include "offer-reply.hxx"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace
{

constexpr size_t kMaxReplyWords = 6;

constexpr std::array<std::pair<std::string_view, std::string_view>, 20> kFolds{{
    {"á", "a"}, {"é", "e"}, {"í", "i"}, {"ó", "o"}, {"ú", "u"}, {"ü", "u"}, {"ñ", "n"},
    {"Á", "a"}, {"É", "e"}, {"Í", "i"}, {"Ó", "o"}, {"Ú", "u"}, {"Ñ", "n"},
    {"¡", " "}, {"¿", " "}, {"…", " "}, {"«", " "}, {"»", " "}, {"“", " "}, {"”", " "},
}};

constexpr std::array<std::string_view, 9> kDeclineEs{
    "no", "ahora no", "dejalo", "da igual", "no hace falta", "luego", "despues", "mejor no", "tranquilo"};
constexpr std::array<std::string_view, 6> kDeclineEn{"no", "not now", "never mind", "later", "nope", "no thanks"};
constexpr std::array<std::string_view, 22> kAcceptEs{
    "si", "vale", "claro", "dale", "venga", "ok", "okay", "porfa", "por favor", "muestrala", "muestramela",
    "muestramelo", "muestrame", "ensename", "ensenamela", "ensenamelo", "a ver", "quiero verla", "ponla", "abrela",
    "de acuerdo", "perfecto"};
constexpr std::array<std::string_view, 12> kAcceptEn{
    "yes", "yeah", "yep", "sure", "ok", "okay", "please", "show me", "go ahead", "let me see", "open it", "show it"};

std::string folded(std::string_view text)
{
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back(' ');
  size_t i = 0;
  while (i < text.size()) {
    const auto fold = std::ranges::find_if(kFolds, [&](const auto& pair) {
      return text.substr(i).starts_with(pair.first);
    });
    if (fold != kFolds.end()) {
      out += fold->second;
      i += fold->first.size();
      continue;
    }
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte >= 'A' && byte <= 'Z')
      out.push_back(static_cast<char>(byte - 'A' + 'a'));
    else if (byte < 0x80 && !((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')))
      out.push_back(' ');
    else
      out.push_back(static_cast<char>(byte));
    ++i;
  }
  out.push_back(' ');
  std::string collapsed;
  collapsed.reserve(out.size());
  for (const char c : out)
    if (c != ' ' || collapsed.empty() || collapsed.back() != ' ')
      collapsed.push_back(c);
  return collapsed;
}

template <size_t N>
bool mentions(const std::string& text, const std::array<std::string_view, N>& phrases)
{
  return std::ranges::any_of(phrases, [&](std::string_view phrase) {
    std::string needle;
    needle.reserve(phrase.size() + 2);
    needle.push_back(' ');
    needle += phrase;
    needle.push_back(' ');
    return text.find(needle) != std::string::npos;
  });
}

}

OfferReply offerReplyOf(std::string_view text, VoiceLang lang)
{
  const std::string normalized = folded(text);
  const auto words = static_cast<size_t>(std::ranges::count(normalized, ' ')) - 1;
  if (words == 0 || words > kMaxReplyWords)
    return OfferReply::Other;
  const bool en = lang == VoiceLang::En;
  const bool declines = en ? mentions(normalized, kDeclineEn) : mentions(normalized, kDeclineEs);
  const bool accepts = en ? mentions(normalized, kAcceptEn) : mentions(normalized, kAcceptEs);
  if (declines == accepts)
    return OfferReply::Other;
  return accepts ? OfferReply::Accept : OfferReply::Decline;
}
