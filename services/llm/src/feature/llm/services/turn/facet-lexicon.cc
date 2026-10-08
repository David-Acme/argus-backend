#include "facet-lexicon.hxx"

#include <text/text-norm.hxx>

#include <algorithm>
#include <array>
#include <span>

namespace turn
{

namespace
{

constexpr std::array<std::string_view, 2> kCameraEs{"camara", "camaras"};
constexpr std::array<std::string_view, 2> kCameraEn{"camera", "cameras"};
constexpr std::array<std::string_view, 7> kAgendaEs{"agenda", "calendario", "pendiente", "pendientes",
                                                    "evento", "eventos", "citas"};
constexpr std::array<std::string_view, 5> kAgendaEn{"agenda", "calendar", "schedule", "pending", "events"};
constexpr std::array<std::string_view, 9> kGuardEs{"vigilancia", "guardia", "alarma",     "seguridad", "modo",
                                                    "armada",     "armado",  "desarmada",  "desarmado"};
constexpr std::array<std::string_view, 6> kGuardEn{"guard", "security", "alarm", "mode", "armed", "disarmed"};
constexpr std::array<std::string_view, 5> kRemindersEs{"recordatorio", "recordatorios", "recuerdame", "avisame", "aviso"};
constexpr std::array<std::string_view, 5> kRemindersEn{"reminder", "reminders", "remind", "remindme", "alert"};
constexpr std::array<std::string_view, 7> kMemoryEs{"memoria", "recuerdas", "acuerdate", "acuerdas",
                                                     "olvida",  "olvidar",   "olvidalo"};
constexpr std::array<std::string_view, 5> kMemoryEn{"memory", "remember", "recall", "forget", "forgot"};
constexpr std::array<std::string_view, 4> kModulesEs{"modulo", "modulos", "componente", "componentes"};
constexpr std::array<std::string_view, 4> kModulesEn{"module", "modules", "component", "components"};

constexpr std::array<std::string_view, 3> kCameraPhrasesEs{"que se ve", "que ves", "que hay en la camara"};
constexpr std::array<std::string_view, 3> kCameraPhrasesEn{"what do you see", "what can you see", "on the camera"};
constexpr std::array<std::string_view, 4> kAgendaPhrasesEs{"que tengo", "mi dia", "mi agenda", "esta tarde"};
constexpr std::array<std::string_view, 2> kAgendaPhrasesEn{"do i have", "this afternoon"};
constexpr std::array<std::string_view, 2> kGuardPhrasesEs{"modo de", "casa armada"};
constexpr std::array<std::string_view, 1> kGuardPhrasesEn{"house armed"};
constexpr std::array<std::string_view, 2> kMemoryPhrasesEs{"te acuerdas", "que sabes"};
constexpr std::array<std::string_view, 2> kMemoryPhrasesEn{"do you remember", "what do you know"};
constexpr std::array<std::string_view, 1> kNonePhrases{""};

constexpr std::array<FacetTable, 2> kTables{{
    {.language = "es",
     .words = {std::span<const std::string_view>(kCameraEs), std::span<const std::string_view>(kAgendaEs),
               std::span<const std::string_view>(kGuardEs), std::span<const std::string_view>(kRemindersEs),
               std::span<const std::string_view>(kMemoryEs), std::span<const std::string_view>(kModulesEs)},
     .phrases = {std::span<const std::string_view>(kCameraPhrasesEs), std::span<const std::string_view>(kAgendaPhrasesEs),
                 std::span<const std::string_view>(kGuardPhrasesEs), std::span<const std::string_view>(kNonePhrases),
                 std::span<const std::string_view>(kMemoryPhrasesEs), std::span<const std::string_view>(kNonePhrases)}},
    {.language = "en",
     .words = {std::span<const std::string_view>(kCameraEn), std::span<const std::string_view>(kAgendaEn),
               std::span<const std::string_view>(kGuardEn), std::span<const std::string_view>(kRemindersEn),
               std::span<const std::string_view>(kMemoryEn), std::span<const std::string_view>(kModulesEn)},
     .phrases = {std::span<const std::string_view>(kCameraPhrasesEn), std::span<const std::string_view>(kAgendaPhrasesEn),
                 std::span<const std::string_view>(kGuardPhrasesEn), std::span<const std::string_view>(kNonePhrases),
                 std::span<const std::string_view>(kMemoryPhrasesEn), std::span<const std::string_view>(kNonePhrases)}},
}};

const FacetTable& tableFor(std::string_view lang)
{
  for (const FacetTable& table : kTables)
    if (table.language == lang)
      return table;
  return kTables.front();
}

bool namesAny(const std::vector<std::string>& tokens, std::span<const std::string_view> words)
{
  return std::ranges::any_of(words, [&tokens](std::string_view word) {
    return !word.empty() && std::ranges::find(tokens, word) != tokens.end();
  });
}

bool saysAny(const std::string& folded, std::span<const std::string_view> phrases)
{
  return std::ranges::any_of(phrases, [&folded](std::string_view phrase) {
    return !phrase.empty() && folded.find(phrase) != std::string::npos;
  });
}

}

std::string_view contextFacetToString(ContextFacet facet)
{
  switch (facet) {
    case ContextFacet::Camera:
      return "camera";
    case ContextFacet::Agenda:
      return "agenda";
    case ContextFacet::Guard:
      return "guard";
    case ContextFacet::Reminders:
      return "reminders";
    case ContextFacet::Memory:
      return "memory";
    case ContextFacet::Modules:
      return "modules";
    case ContextFacet::Count:
      break;
  }
  return {};
}

ContextFacet contextFacetFromString(std::string_view name)
{
  for (std::size_t index = 0; index < kFacetCount; ++index) {
    const auto facet = static_cast<ContextFacet>(index);
    if (contextFacetToString(facet) == name)
      return facet;
  }
  return ContextFacet::Count;
}

std::span<const FacetTable> facetTables()
{
  return kTables;
}

std::vector<ContextFacet> facetsInText(std::string_view text, std::string_view lang)
{
  const FacetTable& table = tableFor(lang);
  const std::string folded = text_norm::whitespace(text_norm::stripAccents(std::string(text)), true);
  const std::vector<std::string> tokens = text_norm::words(folded, 3);
  std::vector<ContextFacet> facets;
  for (std::size_t index = 0; index < kFacetCount; ++index)
    if (namesAny(tokens, table.words[index]) || saysAny(folded, table.phrases[index]))
      facets.push_back(static_cast<ContextFacet>(index));
  return facets;
}

std::optional<ContextFacet> facetForTool(std::string_view tool)
{
  const auto startsWith = [tool](std::string_view prefix) { return tool.starts_with(prefix); };
  if (startsWith("calendar."))
    return ContextFacet::Agenda;
  if (tool == "reminder.list" || tool == "memory.remind")
    return ContextFacet::Reminders;
  if (startsWith("memory."))
    return ContextFacet::Memory;
  if (startsWith("modules."))
    return ContextFacet::Modules;
  if (tool == "app.show_camera")
    return ContextFacet::Camera;
  if (tool == "app.set_guard_mode")
    return ContextFacet::Guard;
  return std::nullopt;
}

}
