#include "unheard-reply.hxx"

#include <text/secret-phrase.hxx>

#include <array>

namespace
{

constexpr std::array<std::string_view, 3> kSpanishLines{
    "Perdona, no te entendí. ¿Me lo repites?",
    "No te escuché bien. ¿Puedes repetirlo?",
    "Disculpa, no te entendí. ¿Otra vez?"};

constexpr std::array<std::string_view, 3> kEnglishLines{
    "Sorry, I did not catch that. Could you say it again?",
    "I missed that. Can you repeat it?",
    "Sorry, what was that?"};

}

bool carriesWords(std::string_view transcript)
{
  return !secret_phrase::words(transcript).empty();
}

std::string_view UnheardRotation::next(VoiceLang lang)
{
  const auto& lines = lang == VoiceLang::En ? kEnglishLines : kSpanishLines;
  const std::string_view line = lines.at((seed_ + spoken_) % lines.size());
  ++spoken_;
  return line;
}
