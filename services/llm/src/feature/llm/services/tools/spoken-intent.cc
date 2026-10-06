#include "spoken-intent.hxx"

#include <feature/llm/services/tools/app-command.hxx>

#include <text/text-norm.hxx>

#include <algorithm>
#include <array>
#include <string_view>

namespace spoken_intent
{

namespace
{
constexpr size_t kLongestAnswer = 8;

constexpr auto kYesStarters = std::to_array<std::string_view>({
    "si",       "claro", "dale",  "ok",     "okay",     "vale",     "listo",  "perfecto",
    "confirmo", "confirmado", "adelante", "hazlo", "seguro", "obvio", "exacto", "afirmativo",
    "correcto", "yes",   "yeah",  "yep",    "sure",     "confirm",  "go",     "do"});

constexpr auto kRefusals = std::to_array<std::string_view>({
    "no",    "nunca", "tampoco", "cancela", "cancelar", "espera", "para", "detente",
    "wait",  "stop",  "not",     "dont",    "don",      "never",  "cancel", "nope"});

constexpr auto kHesitations = std::to_array<std::string_view>({"pero",   "but",    "antes", "primero",
                                                       "aunque", "before", "first", "however"});

constexpr auto kEnableVerbs = std::to_array<std::string_view>({
    "activa",   "activar",  "activalo", "activala", "enciende", "encender", "instala",  "instalar", "habilita",
    "habilitar", "prende",  "ponlo",    "enable",   "activate", "install",  "add",      "turn",     "switch"});

constexpr auto kRequestVerbs = std::to_array<std::string_view>({"pide",    "pidele", "pedirle", "pedir",
                                                        "solicita", "solicitar", "ask", "request"});

template <size_t N>
bool hasAny(const std::vector<std::string>& words, const std::array<std::string_view, N>& wanted)
{
  return std::ranges::any_of(words, [&wanted](const std::string& word) {
    return std::ranges::find(wanted, word) != wanted.end();
  });
}

bool namesModule(const std::vector<std::string>& words, const ModuleFlag& module)
{
  const auto mentions = [&words](const std::string& name) {
    const auto parts = spokenWords(name);
    return !parts.empty() && std::ranges::all_of(parts, [&words](const std::string& part) {
             return std::ranges::find(words, part) != words.end();
           });
  };
  return std::ranges::find(words, module.id) != words.end() || mentions(module.name.es) ||
         mentions(module.name.en);
}
}

bool affirms(const std::string& utterance)
{
  const auto words = spokenWords(utterance);
  if (words.empty() || words.size() > kLongestAnswer)
    return false;
  if (std::ranges::find(kYesStarters, words.front()) == kYesStarters.end())
    return false;
  return !hasAny(words, kRefusals) && !hasAny(words, kHesitations);
}

bool asksToEnable(const ModuleMention& mention)
{
  const auto words = spokenWords(mention.utterance);
  return namesModule(words, mention.module) && hasAny(words, kEnableVerbs) && !hasAny(words, kRefusals);
}

bool asksToRequest(const ModuleMention& mention)
{
  const auto words = spokenWords(mention.utterance);
  return namesModule(words, mention.module) && hasAny(words, kRequestVerbs) && !hasAny(words, kRefusals);
}

}
