#include "visitor-consent.hxx"

#include <feature/watch/wake-words.hxx>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr std::array kYes = std::to_array<std::string_view>(
    {"si",        "sii",        "claro",     "por supuesto", "de acuerdo", "esta bien", "ta bien", "dale",
     "ya",        "ya pues",    "ya ps",     "listo",        "acepto",     "con gusto", "normal",  "chevere",
     "de una",    "ok",         "okay",      "okey",         "vale",       "bueno",     "yes",     "yeah",
     "yep",       "sure",       "of course", "i agree",      "go ahead",   "fine",      "alright", "all right",
     "that s ok", "thats fine", "ok then"});

constexpr std::array kNo = std::to_array<std::string_view>(
    {"no",         "nop",      "nones",       "nel",       "para nada",  "mejor no",    "no gracias", "prefiero que no",
     "no quiero",  "ni hablar", "de ninguna manera", "nah", "nope",      "no thanks",   "no thank you",
     "i don t",    "i do not",  "rather not", "don t",     "do not"});

constexpr std::array kMinorPhrases = std::to_array<std::string_view>(
    {"soy menor",     "soy menor de edad", "soy un nino",   "soy una nina", "soy nino",     "soy nina",
     "soy chiquito",  "soy chiquita",      "soy un chico",  "soy una chica", "soy pequeno", "soy pequena",
     "i m a minor",   "im a minor",        "i am a minor",  "i m a kid",    "im a kid",     "i am a kid",
     "i m a child",   "im a child",        "i am a child",  "i m underage", "i am underage"});

constexpr std::array kAgeWordsEs = std::to_array<std::string_view>(
    {"uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve", "diez", "once", "doce", "trece",
     "catorce", "quince", "dieciseis", "diecisiete"});

constexpr std::array kAgeWordsEn = std::to_array<std::string_view>(
    {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen",
     "fourteen", "fifteen", "sixteen", "seventeen"});

constexpr std::array kFiller = std::to_array<std::string_view>(
    {"pues", "ps", "gracias", "senor", "senora", "senorita", "joven", "amigo", "amiga", "por favor", "porfa",
     "porfavor", "please", "thanks", "thank you", "oye", "oiga", "hola", "eh", "este", "mm", "mmm", "ah", "nomas"});

constexpr std::array kYesIdioms = std::to_array<std::string_view>(
    {"no hay problema", "no hay drama", "no problem", "why not", "por que no"});

constexpr int kAdultAge = 18;

bool hasPhrase(const std::string& padded, std::span<const std::string_view> phrases)
{
  return std::ranges::any_of(phrases, [&padded](std::string_view phrase) {
    return padded.find(" " + std::string(phrase) + " ") != std::string::npos;
  });
}

std::vector<std::string> answerWords(const ConsentAnswerInput& input)
{
  std::vector<std::string> words;
  for (WakeWord& word : wakeWordsOf(input.text))
    if (!isWakeWord(word.folded, input.wakeMaxDistance))
      words.push_back(std::move(word.folded));
  return words;
}

bool childAge(std::string_view word)
{
  if (!word.empty() && std::ranges::all_of(word, [](char c) { return c >= '0' && c <= '9'; })) {
    if (word.size() > 2)
      return false;
    int age = 0;
    for (const char c : word)
      age = (age * 10) + (c - '0');
    return age > 0 && age < kAdultAge;
  }
  return std::ranges::find(kAgeWordsEs, word) != kAgeWordsEs.end() ||
         std::ranges::find(kAgeWordsEn, word) != kAgeWordsEn.end();
}

bool statesChildAge(const std::vector<WakeWord>& words)
{
  for (size_t index = 0; index + 2 < words.size(); ++index) {
    const std::string& first = words[index].folded;
    const std::string& unit = words[index + 2].folded;
    if (!childAge(words[index + 1].folded))
      continue;
    if (first == "tengo" && (unit == "anos" || unit == "ano" || unit == "anitos"))
      return true;
    if ((first == "m" || first == "am" || first == "im") && (unit == "years" || unit == "year"))
      return true;
  }
  return false;
}

}

ConsentAnswer consentAnswer(const ConsentAnswerInput& input)
{
  const std::vector<std::string> words = answerWords(input);
  if (words.empty() || std::cmp_greater(words.size(), input.maxWords))
    return ConsentAnswer::None;
  std::string padded = " ";
  for (const std::string& word : words)
    padded += word + " ";
  for (const std::string_view idiom : kYesIdioms) {
    const std::string needle = " " + std::string(idiom) + " ";
    for (size_t at = padded.find(needle); at != std::string::npos; at = padded.find(needle))
      padded.replace(at, needle.size(), " si ");
  }
  if (hasPhrase(padded, kNo))
    return ConsentAnswer::No;
  if (!hasPhrase(padded, kYes))
    return ConsentAnswer::None;
  std::string rest = padded;
  for (const auto& list : {std::span<const std::string_view>(kYes), std::span<const std::string_view>(kFiller)})
    for (const std::string_view phrase : list) {
      const std::string needle = " " + std::string(phrase) + " ";
      for (size_t at = rest.find(needle); at != std::string::npos; at = rest.find(needle))
        rest.replace(at, needle.size(), " ");
    }
  return std::ranges::all_of(rest, [](char c) { return c == ' '; }) ? ConsentAnswer::Yes : ConsentAnswer::None;
}

bool minorStatement(std::string_view text)
{
  const std::string padded = " " + foldText(text) + " ";
  if (hasPhrase(padded, kMinorPhrases))
    return true;
  return statesChildAge(wakeWordsOf(text));
}

ConsentTurn consentTurn(const ConsentTurnInput& input)
{
  ConsentTurn out{.consent = input.consent, .toAssistant = false, .askedAgain = false, .minorStated = false, .line = {}};
  if (minorStatement(input.text)) {
    out.consent = VisitorConsent::Declined;
    out.minorStated = true;
    out.line = std::string(safetyOnlyLine(input.lang));
    return out;
  }
  if (input.consent == VisitorConsent::Granted) {
    out.toAssistant = true;
    return out;
  }
  if (input.consent == VisitorConsent::Declined) {
    out.line = std::string(safetyOnlyLine(input.lang));
    return out;
  }
  if (input.beforeQuestion)
    return out;
  switch (consentAnswer({.text = input.text, .maxWords = input.maxWords, .wakeMaxDistance = input.wakeMaxDistance})) {
    case ConsentAnswer::Yes:
      out.consent = VisitorConsent::Granted;
      out.line = std::string(consentThanks(input.lang));
      return out;
    case ConsentAnswer::No:
      out.consent = VisitorConsent::Declined;
      out.line = std::string(consentDeclined(input.lang)) + " " + std::string(safetyOnlyLine(input.lang));
      return out;
    case ConsentAnswer::None:
      break;
  }
  out.line = std::string(safetyOnlyLine(input.lang));
  if (input.asked < input.maxAsks) {
    out.askedAgain = true;
    out.line += " " + std::string(consentAskAgain(input.lang));
  }
  return out;
}

std::string_view consentQuestion(VoiceLang lang)
{
  if (lang == VoiceLang::En)
    return "Hello, I'm this home's assistant. This conversation is kept. May I remember you next time? Please "
           "answer yes or no.";
  return "Hola, soy el asistente de esta casa. Esta conversación se guarda. ¿Me permite recordarle para la próxima "
         "vez? Responda sí o no.";
}

std::string_view consentAskAgain(VoiceLang lang)
{
  if (lang == VoiceLang::En)
    return "To help you with anything else, may I remember you? Please answer yes or no.";
  return "Para ayudarle con algo más, ¿me permite recordarle? Responda sí o no.";
}

std::string_view consentThanks(VoiceLang lang)
{
  if (lang == VoiceLang::En)
    return "Thank you. How can I help you?";
  return "Gracias. ¿En qué le puedo ayudar?";
}

std::string_view consentDeclined(VoiceLang lang)
{
  if (lang == VoiceLang::En)
    return "Understood, I won't remember you.";
  return "Entendido, no le recordaré.";
}

std::string_view safetyOnlyLine(VoiceLang lang)
{
  if (lang == VoiceLang::En)
    return "I can pass your message on to the household. If this is an emergency, say \"help\".";
  return "Puedo dejarle su mensaje a la casa. Si es una emergencia, diga «auxilio».";
}

std::string_view consentGrantedNote(VoiceLang lang)
{
  if (lang == VoiceLang::En)
    return "The visitor agreed to be remembered. Keep to the visitor set: never confirm or deny that anyone is home.";
  return "El visitante aceptó que lo recuerdes. Sigue con lo de visitante: nunca confirmes ni niegues si hay alguien "
         "en casa.";
}
