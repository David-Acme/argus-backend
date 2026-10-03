#include <feature/camera-notification/services/camera-notification-copy.hxx>

#include <array>
#include <cctype>
#include <string_view>
#include <vector>

namespace
{

struct ObjectWords
{
  std::string_view esOne;
  std::string_view esMany;
  std::string_view enOne;
  std::string_view enMany;
};

ObjectWords wordsFor(std::string_view objectClass)
{
  constexpr std::array<std::string_view, 6> kVehicles{
      "car", "truck", "bus", "motorcycle", "bicycle", "vehicle"};
  constexpr std::array<std::string_view, 4> kAnimals{"dog", "cat", "bird",
                                                     "animal"};
  if (objectClass == "person")
    return {.esOne = "persona", .esMany = "personas", .enOne = "person",
            .enMany = "people"};
  for (const auto vehicle : kVehicles) {
    if (vehicle == objectClass)
      return {.esOne = "vehículo", .esMany = "vehículos", .enOne = "vehicle",
              .enMany = "vehicles"};
  }
  for (const auto animal : kAnimals) {
    if (animal == objectClass)
      return {.esOne = "animal", .esMany = "animales", .enOne = "animal",
              .enMany = "animals"};
  }
  return {.esOne = "objeto", .esMany = "objetos", .enOne = "object",
          .enMany = "objects"};
}

std::string cameraLabel(const FallbackNotice& notice, bool english)
{
  if (!notice.cameraName.empty())
    return notice.cameraName;
  return (english ? "Camera " : "Cámara ") + std::to_string(notice.cameraId);
}

std::string countsPhrase(const std::map<std::string, int>& suppressed,
                         bool english)
{
  std::map<std::string, std::pair<ObjectWords, int>> grouped;
  for (const auto& [objectClass, count] : suppressed) {
    if (count <= 0)
      continue;
    const ObjectWords words = wordsFor(objectClass);
    auto& entry = grouped[std::string(words.enMany)];
    entry.first = words;
    entry.second += count;
  }
  std::vector<std::string> parts;
  for (const auto& [key, entry] : grouped) {
    const auto& [words, count] = entry;
    const std::string_view noun =
        english ? (count == 1 ? words.enOne : words.enMany)
                : (count == 1 ? words.esOne : words.esMany);
    parts.push_back(std::to_string(count) + " " + std::string(noun));
  }
  std::string text;
  for (size_t index = 0; index < parts.size(); ++index) {
    if (index > 0)
      text += index + 1 == parts.size() ? (english ? " and " : " y ") : ", ";
    text += parts[index];
  }
  return text;
}

}

std::string
camera_notification_copy::normalizeLang(const LangPreference& preference)
{
  const auto prefix = [](std::string_view value) {
    std::string code;
    for (const char step : value.substr(0, 2))
      code.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(step))));
    return code;
  };
  std::string code = prefix(preference.requested);
  if (code == "es" || code == "en")
    return code;
  return prefix(preference.fallback) == "en" ? "en" : "es";
}

FallbackText camera_notification_copy::render(const FallbackNotice& notice,
                                              std::string_view lang)
{
  const bool english =
      normalizeLang({.requested = lang, .fallback = "es"}) == "en";
  const std::string camera = cameraLabel(notice, english);
  if (notice.kind == FallbackNoticeKind::Digest) {
    const std::string counts = countsPhrase(notice.suppressed, english);
    return {.title = (english ? "While the guard was offline · "
                              : "Mientras la vigilancia no respondía · ") +
                     camera,
            .body = english ? "The camera saw " + counts +
                                  " that weren't alerted one by one."
                            : "La cámara detectó " + counts +
                                  " que no se avisaron uno a uno."};
  }
  const bool zone = notice.rule == "person_in_alert_zone";
  const std::string headline =
      zone ? (english ? "Person in the alert zone" : "Persona en la zona de alerta")
           : (english ? "Camera alert" : "Aviso de la cámara");
  return {.title = headline + " · " + camera,
          .body = english
                      ? "Argus's guard isn't responding, so this alert comes "
                        "straight from the camera. Take a look at the view."
                      : "La vigilancia de Argus no responde, así que este aviso "
                        "llega directo de la cámara. Echa un vistazo a la "
                        "imagen."};
}
