#include "sight-command.hxx"

#include <feature/llm/services/tools/app-command.hxx>

#include <algorithm>
#include <array>
#include <charconv>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using Words = std::vector<std::string>;

constexpr std::array<std::string_view, 20> kAimVerbs{
    "mira", "mirar", "gira", "girar", "girala", "apunta", "apuntala", "voltea", "voltear", "mueve",
    "muevela", "enfoca", "enfocala", "look", "turn", "point", "pan", "aim", "move", "face"};
constexpr std::array<std::string_view, 7> kTargetMarkers{"hacia", "al", "a", "to", "towards", "at", "en"};
constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kDirections{{{"izquierda", "left"},
                                                                                    {"left", "left"},
                                                                                    {"derecha", "right"},
                                                                                    {"right", "right"},
                                                                                    {"arriba", "up"},
                                                                                    {"up", "up"},
                                                                                    {"abajo", "down"},
                                                                                    {"down", "down"}}};
constexpr std::array<std::string_view, 5> kHomeWords{"vigilancia", "casa", "inicio", "home", "origen"};
constexpr std::array<std::string_view, 12> kFillers{"de", "del", "la", "el", "los", "las", "the", "su", "tu",
                                                    "posicion", "position", "por"};
constexpr std::array<std::string_view, 6> kTails{"favor", "please", "ahora", "now", "ya", "porfa"};
constexpr std::array<std::string_view, 4> kCameraWords{"camara", "camera", "camaras", "cameras"};
constexpr std::array<std::string_view, 6> kFutureWords{"voy", "vamos", "going", "will", "gonna", "ire"};
constexpr std::array<std::string_view, 16> kMaintenanceWords{
    "limpiar", "limpio", "limpiare", "mantenimiento", "arreglar", "reparar", "mover", "cambiar",
    "revisar", "clean", "fix", "repair", "maintenance", "service", "check", "adjust"};
constexpr std::array<std::string_view, 7> kDoneWords{"termine", "acabe", "terminado", "listo", "done",
                                                     "finished", "fin"};
constexpr std::array<std::string_view, 10> kDoneContext{"mantenimiento", "camara", "camaras", "limpiar",
                                                       "limpieza", "maintenance", "camera", "cameras",
                                                       "cleaning", "arreglo"};
constexpr std::array<std::string_view, 6> kStatusPhrases{"por que me avisaste",  "quien movio la camara",
                                                        "quien tapo la camara",  "por que perdiste la camara",
                                                        "who moved the camera",  "why did you warn me"};

bool isAny(std::string_view word, std::span<const std::string_view> set)
{
  return std::ranges::find(set, word) != set.end();
}

bool hasAny(const Words& words, std::span<const std::string_view> set)
{
  return std::ranges::any_of(words, [set](const std::string& word) { return isAny(word, set); });
}

struct WordSpan
{
  size_t from{0};
  size_t to{0};
};

std::string joined(const Words& words, WordSpan span)
{
  std::string text;
  for (size_t index = span.from; index < span.to && index < words.size(); ++index) {
    if (isAny(words[index], kFillers) || isAny(words[index], kTails) || isAny(words[index], kCameraWords))
      continue;
    text += text.empty() ? words[index] : " " + words[index];
  }
  return text;
}

std::optional<std::string_view> directionOf(const Words& words)
{
  for (const auto& word : words)
    for (const auto& [spoken, direction] : kDirections)
      if (word == spoken)
        return direction;
  return std::nullopt;
}

tools::ToolCall callNamed(std::string name)
{
  tools::ToolCall call;
  call.name = std::move(name);
  call.arguments = Json::Value(Json::objectValue);
  return call;
}

std::optional<tools::ToolCall> aimCommand(const Words& words)
{
  const auto verb = std::ranges::find_if(words, [](const std::string& word) { return isAny(word, kAimVerbs); });
  if (verb == words.end())
    return std::nullopt;
  const auto verbAt = static_cast<size_t>(verb - words.begin());
  const auto cameraWord = std::ranges::find_if(words, [](const std::string& word) { return isAny(word, kCameraWords); });
  const bool namesCamera = cameraWord != words.end();
  size_t markerAt = words.size();
  for (size_t index = words.size(); index-- > verbAt + 1;) {
    if (isAny(words[index], kTargetMarkers)) {
      markerAt = index;
      break;
    }
  }
  const bool towards = markerAt < words.size() && (words[markerAt] == "hacia" || words[markerAt] == "towards");
  if (!namesCamera && !towards)
    return std::nullopt;
  std::string camera;
  if (namesCamera) {
    const auto cameraAt = static_cast<size_t>(cameraWord - words.begin());
    camera = joined(words, {.from = cameraAt + 1, .to = markerAt > cameraAt ? markerAt : words.size()});
    if (camera.empty() && cameraAt > verbAt + 1)
      camera = joined(words, {.from = verbAt + 1, .to = cameraAt});
  }
  if (const auto direction = directionOf(words)) {
    tools::ToolCall call = callNamed("camera.look");
    call.arguments["camera"] = camera;
    call.arguments["direction"] = std::string(*direction);
    return call;
  }
  if (markerAt == words.size())
    return std::nullopt;
  const std::string target = joined(words, {.from = markerAt + 1, .to = words.size()});
  if (target.empty())
    return std::nullopt;
  if (isAny(target, kHomeWords)) {
    tools::ToolCall call = callNamed("camera.look");
    call.arguments["camera"] = camera;
    call.arguments["direction"] = "home";
    return call;
  }
  tools::ToolCall call = callNamed("camera.goto_preset");
  call.arguments["camera"] = camera;
  call.arguments["preset"] = target;
  return call;
}

int minutesOf(const Words& words)
{
  for (size_t index = 0; index + 1 < words.size(); ++index) {
    int value = 0;
    const std::string& word = words[index];
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
    if (error != std::errc{} || end != word.data() + word.size())
      continue;
    const std::string& unit = words[index + 1];
    if (unit.starts_with("min"))
      return value;
    if (unit.starts_with("hora") || unit.starts_with("hour"))
      return value * 60;
  }
  if (std::ranges::find(words, "media") != words.end() && std::ranges::find(words, "hora") != words.end())
    return 30;
  return 0;
}

std::optional<tools::ToolCall> maintenanceStart(const Words& words)
{
  if (!hasAny(words, kFutureWords) || !hasAny(words, kMaintenanceWords))
    return std::nullopt;
  const auto cameraWord = std::ranges::find_if(words, [](const std::string& word) { return isAny(word, kCameraWords); });
  if (cameraWord == words.end() && std::ranges::find(words, "mantenimiento") == words.end() &&
      std::ranges::find(words, "maintenance") == words.end())
    return std::nullopt;
  tools::ToolCall call = callNamed("guard.maintenance_start");
  std::string camera;
  if (cameraWord != words.end()) {
    const auto cameraAt = static_cast<size_t>(cameraWord - words.begin());
    size_t end = words.size();
    for (size_t index = cameraAt + 1; index < words.size(); ++index)
      if (words[index] == "por" || words[index] == "for" || words[index] == "durante") {
        end = index;
        break;
      }
    camera = joined(words, {.from = cameraAt + 1, .to = end});
  }
  call.arguments["camera"] = camera;
  if (const int minutes = minutesOf(words); minutes > 0)
    call.arguments["minutes"] = minutes;
  return call;
}

std::optional<tools::ToolCall> maintenanceEnd(const Words& words)
{
  if (!hasAny(words, kDoneWords) || !hasAny(words, kDoneContext) || hasAny(words, kFutureWords))
    return std::nullopt;
  return callNamed("guard.maintenance_end");
}

std::optional<tools::ToolCall> maintenanceStatus(const Words& words)
{
  std::string text;
  for (const auto& word : words)
    text += text.empty() ? word : " " + word;
  const bool asked = std::ranges::any_of(kStatusPhrases, [&text](std::string_view phrase) {
    return text.find(phrase) != std::string::npos;
  });
  if (!asked)
    return std::nullopt;
  return callNamed("guard.maintenance_status");
}
}

std::optional<tools::ToolCall> sightCommandFor(const std::string& utterance)
{
  const Words words = spokenWords(utterance);
  if (words.empty())
    return std::nullopt;
  if (auto status = maintenanceStatus(words))
    return status;
  if (utterance.find('?') != std::string::npos)
    return std::nullopt;
  if (auto end = maintenanceEnd(words))
    return end;
  if (auto start = maintenanceStart(words))
    return start;
  return aimCommand(words);
}
