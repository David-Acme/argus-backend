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
constexpr std::array<std::string_view, 2> kSeeVerbs{"ver", "see"};
constexpr std::array<std::string_view, 9> kNameFillers{"de", "del", "la", "el", "los", "las", "en", "the", "a"};
constexpr std::array<std::string_view, 6> kNameTails{"por", "favor", "ahora", "please", "now", "ya"};
constexpr std::array<std::string_view, 12> kOpenVerbs{
    "abre", "abreme", "ve", "vamos", "llevame", "muestrame", "ensename", "open", "go", "take", "show", "pon"};
constexpr std::array<std::pair<std::string_view, std::string_view>, 15> kScreens{{{"agenda", "agenda"},
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
                                                                                 {"inicio", "home"}}};

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

std::optional<tools::ToolCall> guardMode(const Words& words)
{
  if (!hasAny(words, kGuardContext) || !hasAny(words, kGuardVerbs))
    return std::nullopt;
  const auto modeWord = std::ranges::find_if(words, [](const std::string& word) {
    return word == "modo" || word == "mode";
  });
  if (modeWord != words.end() && std::next(modeWord) != words.end()) {
    if (const auto mode = lookup(kModes, *std::next(modeWord)))
      return callOf({.name = "app.set_guard_mode", .argument = "mode", .value = std::string(*mode)});
  }
  for (const auto& word : words) {
    if (const auto mode = lookup(kModes, word))
      return callOf({.name = "app.set_guard_mode", .argument = "mode", .value = std::string(*mode)});
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
