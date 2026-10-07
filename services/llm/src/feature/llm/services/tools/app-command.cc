#include "app-command.hxx"

#include <text/text-norm.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using Words = std::vector<std::string>;

constexpr std::array<std::string_view, 9> kGuardContext{
    "vigilancia", "modo", "guardia", "alarma", "seguridad", "guard", "mode", "security", "alarm"};
constexpr std::array<std::string_view, 17> kGuardVerbs{
    "pon",    "ponla",  "ponlo", "pone",   "poner", "activa", "activala", "cambia", "cambiala",
    "pasa",   "pasala", "set",   "switch", "put",   "turn",   "change",   "activate"};
constexpr std::array<std::pair<std::string_view, std::string_view>, 12> kModes{{{"noche", "night"},
                                                                               {"nocturno", "night"},
                                                                               {"night", "night"},
                                                                               {"fuera", "away"},
                                                                               {"ausente", "away"},
                                                                               {"ausencia", "away"},
                                                                               {"away", "away"},
                                                                               {"casa", "home"},
                                                                               {"home", "home"},
                                                                               {"armado", "armed"},
                                                                               {"armada", "armed"},
                                                                               {"armed", "armed"}}};
constexpr std::array<std::string_view, 14> kShowVerbs{"muestrame", "muestra", "ensename", "abre",
                                                     "abreme",    "pon",     "ponme",    "checa",
                                                     "chequea",   "revisa",  "show",     "open",
                                                     "display",   "check"};
constexpr std::array<std::string_view, 4> kWantVerbs{"quiero", "dejame", "let", "want"};
constexpr std::array<std::string_view, 13> kQuestionOpeners{
    "que", "cual", "como", "esta", "estamos", "sigue", "hay", "is", "what", "which", "how", "are", "does"};
constexpr size_t kTerseCommandWords = 7;
constexpr std::array<std::string_view, 6> kAppTopics{"camara", "camera", "camaras", "cameras", "app", "pantalla"};
constexpr std::array<std::string_view, 33> kClaims{
    "cambie",     "cambiado",   "cambiada",    "active",      "activado",    "activada",   "puse",
    "puesto",     "puesta",     "abri",        "abierto",     "abierta",     "mostre",     "mostrando",
    "desactive",  "desactivado", "desactivada", "configure",  "configurado", "configurada", "listo",
    "hecho",      "changed",    "switched",    "opened",      "showing",     "activated",  "enabled",
    "disabled",   "turned",     "done",        "armado",      "desarmado"};
constexpr std::array<std::string_view, 2> kSeeVerbs{"ver", "see"};
constexpr std::array<std::string_view, 9> kNameFillers{"de", "del", "la", "el", "los", "las", "en", "the", "a"};
constexpr std::array<std::string_view, 6> kNameTails{"por", "favor", "ahora", "please", "now", "ya"};
constexpr std::array<std::string_view, 24> kPlaceNoise{
    "a",   "al",  "para", "to",  "in",  "on",  "of", "for", "y",     "and",   "me",    "voy",
    "nos", "vamos", "ya", "que", "porfa", "todo", "toda", "todos", "todas", "everything", "all", "it"};
constexpr std::array<std::string_view, 12> kOpenVerbs{
    "abre", "abreme", "ve", "vamos", "llevame", "muestrame", "ensename", "open", "go", "take", "show", "pon"};
constexpr std::array<std::pair<std::string_view, std::string_view>, 20> kScreens{{{"agenda", "agenda"},
                                                                                 {"calendario", "agenda"},
                                                                                 {"calendar", "agenda"},
                                                                                 {"proyectos", "projects"},
                                                                                 {"projects", "projects"},
                                                                                 {"camaras", "cameras"},
                                                                                 {"cameras", "cameras"},
                                                                                 {"seguridad", "security"},
                                                                                 {"security", "security"},
                                                                                 {"personas", "people"},
                                                                                 {"people", "people"},
                                                                                 {"ajustes", "settings"},
                                                                                 {"configuracion", "settings"},
                                                                                 {"settings", "settings"},
                                                                                 {"inicio", "home"},
                                                                                 {"notificaciones", "notifications"},
                                                                                 {"notificacion", "notifications"},
                                                                                 {"novedades", "notifications"},
                                                                                 {"notifications", "notifications"},
                                                                                 {"notification", "notifications"}}};

Words wordsOf(const std::string& utterance)
{
  std::string folded = text_norm::stripAccents(utterance);
  for (auto& c : folded) {
    const auto byte = static_cast<unsigned char>(c);
    c = std::isalnum(byte) != 0 ? static_cast<char>(std::tolower(byte)) : ' ';
  }
  Words words;
  size_t at = 0;
  while (at < folded.size()) {
    const size_t begin = folded.find_first_not_of(' ', at);
    if (begin == std::string::npos)
      break;
    const size_t end = std::min(folded.find(' ', begin), folded.size());
    words.push_back(folded.substr(begin, end - begin));
    at = end;
  }
  return words;
}

bool hasAny(const Words& words, std::span<const std::string_view> wanted)
{
  return std::ranges::any_of(words, [wanted](const std::string& word) {
    return std::ranges::find(wanted, word) != wanted.end();
  });
}

template <size_t N>
std::optional<std::string_view> lookup(const std::array<std::pair<std::string_view, std::string_view>, N>& table,
                                       std::string_view word)
{
  const auto found = std::ranges::find(table, word, &std::pair<std::string_view, std::string_view>::first);
  if (found == table.end())
    return std::nullopt;
  return found->second;
}

struct AppCall
{
  std::string name;
  std::string argument;
  std::string value;
};

tools::ToolCall callOf(AppCall app)
{
  tools::ToolCall call;
  call.name = std::move(app.name);
  call.arguments = Json::Value(Json::objectValue);
  call.arguments[app.argument] = app.value;
  return call;
}

bool asksToSee(const Words& words)
{
  if (hasAny(words, kShowVerbs))
    return true;
  for (size_t i = 0; i + 1 < words.size(); ++i) {
    if (std::ranges::find(kWantVerbs, words[i]) == kWantVerbs.end())
      continue;
    const size_t next = words[i + 1] == "me" && i + 2 < words.size() ? i + 2 : i + 1;
    if (std::ranges::find(kSeeVerbs, words[next]) != kSeeVerbs.end())
      return true;
  }
  return false;
}

bool terseCommand(const Words& words)
{
  return words.size() <= kTerseCommandWords &&
         std::ranges::find(kQuestionOpeners, words.front()) == kQuestionOpeners.end();
}

bool isAny(const std::string& word, std::span<const std::string_view> set)
{
  return std::ranges::find(set, word) != set.end();
}

std::string placeHint(const Words& words, size_t modeAt)
{
  std::string hint;
  for (size_t i = 0; i < words.size(); ++i) {
    const std::string& word = words[i];
    if (i == modeAt || word == "modo" || word == "mode" || isAny(word, kGuardContext) ||
        isAny(word, kGuardVerbs) || isAny(word, kNameFillers) || isAny(word, kNameTails) ||
        isAny(word, kPlaceNoise))
      continue;
    hint += hint.empty() ? word : " " + word;
  }
  return hint;
}

tools::ToolCall guardModeCall(const Words& words, size_t modeAt, std::string_view mode)
{
  tools::ToolCall call = callOf({.name = "app.set_guard_mode", .argument = "mode", .value = std::string(mode)});
  if (const std::string hint = placeHint(words, modeAt); !hint.empty())
    call.arguments["environment"] = hint;
  return call;
}

std::optional<tools::ToolCall> guardMode(const Words& words)
{
  if (!hasAny(words, kGuardContext))
    return std::nullopt;
  const auto modeWord = std::ranges::find_if(words, [](const std::string& word) {
    return word == "modo" || word == "mode";
  });
  if (modeWord != words.end() && std::next(modeWord) != words.end() &&
      (hasAny(words, kGuardVerbs) || terseCommand(words))) {
    if (const auto mode = lookup(kModes, *std::next(modeWord)))
      return guardModeCall(words, static_cast<size_t>(std::next(modeWord) - words.begin()), *mode);
  }
  if (!hasAny(words, kGuardVerbs))
    return std::nullopt;
  for (size_t i = 0; i < words.size(); ++i) {
    if (const auto mode = lookup(kModes, words[i]))
      return guardModeCall(words, i, *mode);
  }
  return std::nullopt;
}

std::string cameraName(const Words& words, size_t cameraAt)
{
  size_t last = words.size();
  while (last > cameraAt + 1 && std::ranges::find(kNameTails, words[last - 1]) != kNameTails.end())
    --last;
  std::string name;
  for (size_t i = cameraAt + 1; i < last; ++i) {
    if (name.empty() && std::ranges::find(kNameFillers, words[i]) != kNameFillers.end())
      continue;
    name += name.empty() ? words[i] : " " + words[i];
  }
  if (!name.empty() || words[cameraAt] != "camera")
    return name;
  size_t begin = cameraAt;
  while (begin > 0 && std::ranges::find(kNameFillers, words[begin - 1]) == kNameFillers.end() &&
         std::ranges::find(kShowVerbs, words[begin - 1]) == kShowVerbs.end() &&
         std::ranges::find(kSeeVerbs, words[begin - 1]) == kSeeVerbs.end() && words[begin - 1] != "me")
    --begin;
  for (size_t i = begin; i < cameraAt; ++i)
    name += name.empty() ? words[i] : " " + words[i];
  return name;
}

std::optional<tools::ToolCall> showCamera(const Words& words)
{
  const auto camera = std::ranges::find_if(words, [](const std::string& word) {
    return word == "camara" || word == "camera";
  });
  if (camera == words.end() || !asksToSee(words))
    return std::nullopt;
  return callOf({.name = "app.show_camera",
                 .argument = "camera",
                 .value = cameraName(words, static_cast<size_t>(camera - words.begin()))});
}

std::optional<tools::ToolCall> openScreen(const Words& words)
{
  if (!hasAny(words, kOpenVerbs))
    return std::nullopt;
  for (const auto& word : words) {
    if (const auto screen = lookup(kScreens, word))
      return callOf({.name = "app.open", .argument = "screen", .value = std::string(*screen)});
  }
  return std::nullopt;
}

}

std::vector<std::string> spokenWords(const std::string& utterance)
{
  return wordsOf(utterance);
}

bool isAppTool(std::string_view name)
{
  return name.starts_with("app.");
}

bool asksForAppAction(const std::string& utterance)
{
  if (utterance.find('?') != std::string::npos)
    return false;
  const Words words = wordsOf(utterance);
  if (words.empty() || std::ranges::find(kQuestionOpeners, words.front()) != kQuestionOpeners.end())
    return false;
  const bool screenWord = std::ranges::any_of(words, [](const std::string& word) {
    return lookup(kScreens, word).has_value();
  });
  return hasAny(words, kGuardContext) || hasAny(words, kAppTopics) ||
         (screenWord && hasAny(words, kOpenVerbs));
}

bool namesGuardMode(const std::string& utterance, std::string_view mode)
{
  if (utterance.find('?') != std::string::npos)
    return false;
  const Words words = wordsOf(utterance);
  return hasAny(words, kGuardContext) &&
         std::ranges::any_of(words, [mode](const std::string& word) { return lookup(kModes, word) == mode; });
}

bool claimsAppAction(const std::string& reply)
{
  return hasAny(wordsOf(reply), kClaims);
}

std::optional<tools::ToolCall> appCommandFor(const std::string& utterance)
{
  if (utterance.find('?') != std::string::npos)
    return std::nullopt;
  const Words words = wordsOf(utterance);
  if (words.empty())
    return std::nullopt;
  if (auto call = guardMode(words))
    return call;
  if (auto call = showCamera(words))
    return call;
  return openScreen(words);
}
