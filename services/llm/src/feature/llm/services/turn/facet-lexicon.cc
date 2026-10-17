#include "facet-lexicon.hxx"

#include <text/name-match.hxx>
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
constexpr std::array<std::string_view, 13> kAgendaEs{"agenda", "calendario", "pendiente", "pendientes", "evento",
                                                     "eventos", "cita",      "citas",      "reunion",   "reuniones",
                                                     "entrega", "entregas",  "compromiso"};
constexpr std::array<std::string_view, 8> kAgendaEn{"agenda",  "calendar",  "schedule", "pending",
                                                    "event",   "events",    "meeting",  "appointment"};
constexpr std::array<std::string_view, 48> kGuardEs{
    "vigilancia", "guardia",   "alarma",     "alarmas",    "seguridad", "modo",      "armada",     "armado",
    "desarmada",  "desarmado", "policia",    "policias",   "panico",    "emergencia", "emergencias", "intruso",
    "intrusos",   "ladron",    "ladrones",   "robo",       "robos",     "herido",    "herida",     "sospechoso",
    "sospechosa", "peligro",   "auxilio",    "alerta",     "alertas",   "incidente", "incidentes", "episodio",
    "episodios",  "llegada",   "llegadas",   "llego",      "llegaron",  "llega",     "llegan",     "llegar",
    "entro",      "entraron",  "vino",       "vinieron",   "viene",     "sola",      "solas",      "solo"};
constexpr std::array<std::string_view, 28> kGuardEn{
    "guard",     "security", "alarm",     "mode",     "armed",      "disarmed", "police",   "emergency",
    "intruder",  "intruders", "thief",    "thieves",  "robbery",    "burglary", "injured",  "suspicious",
    "danger",    "alert",    "alerts",    "panic",    "arrival",    "arrivals", "arrive",   "arrived",
    "arrives",   "unknown",  "stranger",  "alone"};
constexpr std::array<std::string_view, 8> kRemindersEs{"recordatorio", "recordatorios", "recuerdame", "avisame",
                                                       "aviso",        "avisos",        "debo",       "obligacion"};
constexpr std::array<std::string_view, 9> kRemindersEn{"reminder", "reminders", "remind", "remindme", "alert",
                                                       "alerts",   "due",       "owe",    "supposed"};
constexpr std::array<std::string_view, 10> kMemoryEs{"memoria", "recuerdas", "acuerdate", "acuerdas",  "olvida",
                                                     "olvidar", "olvidalo",  "recuerdo",  "recuerdos", "recordar"};
constexpr std::array<std::string_view, 6> kMemoryEn{"memory", "remember", "recall", "forget", "forgot", "remindme"};
constexpr std::array<std::string_view, 4> kModulesEs{"modulo", "modulos", "componente", "componentes"};
constexpr std::array<std::string_view, 4> kModulesEn{"module", "modules", "component", "components"};
constexpr std::array<std::string_view, 47> kPeopleEs{
    "hijo",    "hija",     "hijos",    "hijas",     "chibolo",     "chibola",   "nieto",     "nieta",
    "papa",    "mama",     "esposo",   "esposa",    "hermano",     "hermana",   "abuelo",    "abuela",
    "tio",     "tia",      "primo",    "prima",     "sobrino",     "sobrina",   "suegro",    "suegra",
    "cunado",  "cunada",   "familia",  "familiares", "miembro",    "miembros",  "persona",   "personas",
    "gente",   "pareja",   "propietario", "propietaria", "dueno", "duena",     "residente", "residentes",
    "invitado", "invitada", "invitados", "visitante", "visitantes", "visita",   "visitas"};
constexpr std::array<std::string_view, 30> kPeopleEn{
    "son",     "sons",    "daughter", "daughters", "kid",      "kids",     "child",    "children",
    "mother",  "mom",     "mum",      "father",    "dad",      "brother",  "sister",   "husband",
    "wife",    "spouse",  "family",   "parents",   "member",   "members",  "person",   "people",
    "owner",   "resident", "guest",   "visitor",   "visitors", "relative"};
constexpr std::array<std::string_view, 10> kTasksEs{"tarea", "tareas", "quehacer", "quehaceres", "deberes",
                                                    "vence", "vencimiento", "encargo", "encargos", "pendiente"};
constexpr std::array<std::string_view, 9> kTasksEn{"task", "tasks",  "deadline", "due",
                                                   "overdue", "chore", "chores",  "todo", "pending"};
constexpr std::array<std::string_view, 4> kProjectsEs{"proyecto", "proyectos", "obra", "obras"};
constexpr std::array<std::string_view, 4> kProjectsEn{"project", "projects", "renovation", "renovations"};

constexpr std::array<std::string_view, 9> kCameraPhrasesEs{"que se ve", "que ves", "que hay en la camara",
                                                           "muestrame", "muestra la camara", "puedes ver",
                                                           "puede ver", "puedo ver", "ver la entrada"};
constexpr std::array<std::string_view, 7> kCameraPhrasesEn{"what do you see", "what can you see", "on the camera",
                                                           "show me", "can you see", "can i see", "front door camera"};
constexpr std::array<std::string_view, 14> kAgendaPhrasesEs{"que tengo", "mi dia",  "mi agenda", "esta tarde",
                                                            "a que hora", "que hora", "que dia",   "cuando es",
                                                            "cuando era", "que tengo hoy", "para cuando", "que hay hoy",
                                                            "que hay",    "manana que"};
constexpr std::array<std::string_view, 9> kAgendaPhrasesEn{"do i have", "this afternoon", "what time", "which day",
                                                           "what day",  "when is",        "what do i have", "what about",
                                                           "tomorrow"};
constexpr std::array<std::string_view, 26> kGuardPhrasesEs{
    "modo de",   "casa armada", "quien fue",  "quien es",  "que paso",   "que ha pasado", "donde esta",
    "donde estan", "que hago",  "llama a la policia", "se fue", "ya se fue", "se marcho", "ya no esta",
    "llego alguien", "vino solo", "vino sola", "esta armada", "hay alguien", "alguien entro",
    "como esta todo", "como esta la casa", "que esta haciendo", "a que hora", "que hora", "fue alguien"};
constexpr std::array<std::string_view, 26> kGuardPhrasesEn{
    "house armed", "who was it",  "what happened", "where is he",    "where is she",  "where are they",
    "what should i do", "what do i do", "call the police", "is the alarm", "did they leave", "did he leave",
    "did she leave", "come home", "came home",     "who came",       "who arrived",   "how is everything",
    "come in", "came in",
    "what are they doing", "what is he doing", "what is she doing", "what time", "was she alone", "by herself"};
constexpr std::array<std::string_view, 9> kRemindersPhrasesEs{"tengo que",       "cuando toca", "cuando tengo que",
                                                              "cuando debo",     "que dia",     "cuando es el pago",
                                                              "a que hora tengo que", "a que hora debo", "me toca"};
constexpr std::array<std::string_view, 8> kRemindersPhrasesEn{"i have to", "i need to", "supposed to", "which day",
                                                              "what day", "when do i", "when am i", "i'm supposed to"};
constexpr std::array<std::string_view, 5> kMemoryPhrasesEs{"te acuerdas", "que sabes", "te acuerdas de", "sabes de",
                                                           "me acuerdo"};
constexpr std::array<std::string_view, 4> kMemoryPhrasesEn{"do you remember", "what do you know", "do you know",
                                                           "do you recall"};
constexpr std::array<std::string_view, 12> kPeoplePhrasesEs{"mi hijo",     "mi hija",     "mi mama",    "mi papa",
                                                            "mi esposa",   "mi esposo",   "mi familia", "mi chibolo",
                                                            "mi chibola",  "quien es",    "quienes son", "quienes estan"};
constexpr std::array<std::string_view, 10> kPeoplePhrasesEn{"my son",     "my daughter", "my kids",   "my husband",
                                                            "my wife",    "my family",   "who is",    "who are",
                                                            "who is home", "with whom"};
constexpr std::array<std::string_view, 6> kTasksPhrasesEs{"que tengo pendiente", "tengo pendiente", "hasta cuando",
                                                          "cuando vence",        "que me falta",    "que tengo que hacer"};
constexpr std::array<std::string_view, 5> kTasksPhrasesEn{"when is it due", "when is the task", "due date",
                                                          "have pending", "what do i have pending"};
constexpr std::array<std::string_view, 2> kProjectsPhrasesEs{"en que va", "como va el proyecto"};
constexpr std::array<std::string_view, 2> kProjectsPhrasesEn{"how is the project", "how is the renovation"};
constexpr std::array<std::string_view, 1> kNonePhrases{""};

constexpr std::array<FacetTable, 2> kTables{{
    {.language = "es",
     .words = {std::span<const std::string_view>(kCameraEs), std::span<const std::string_view>(kAgendaEs),
               std::span<const std::string_view>(kGuardEs), std::span<const std::string_view>(kRemindersEs),
               std::span<const std::string_view>(kMemoryEs), std::span<const std::string_view>(kModulesEs),
               std::span<const std::string_view>(kPeopleEs), std::span<const std::string_view>(kTasksEs),
               std::span<const std::string_view>(kProjectsEs)},
     .phrases = {std::span<const std::string_view>(kCameraPhrasesEs), std::span<const std::string_view>(kAgendaPhrasesEs),
                 std::span<const std::string_view>(kGuardPhrasesEs), std::span<const std::string_view>(kRemindersPhrasesEs),
                 std::span<const std::string_view>(kMemoryPhrasesEs), std::span<const std::string_view>(kNonePhrases),
                 std::span<const std::string_view>(kPeoplePhrasesEs), std::span<const std::string_view>(kTasksPhrasesEs),
                 std::span<const std::string_view>(kProjectsPhrasesEs)}},
    {.language = "en",
     .words = {std::span<const std::string_view>(kCameraEn), std::span<const std::string_view>(kAgendaEn),
               std::span<const std::string_view>(kGuardEn), std::span<const std::string_view>(kRemindersEn),
               std::span<const std::string_view>(kMemoryEn), std::span<const std::string_view>(kModulesEn),
               std::span<const std::string_view>(kPeopleEn), std::span<const std::string_view>(kTasksEn),
               std::span<const std::string_view>(kProjectsEn)},
     .phrases = {std::span<const std::string_view>(kCameraPhrasesEn), std::span<const std::string_view>(kAgendaPhrasesEn),
                 std::span<const std::string_view>(kGuardPhrasesEn), std::span<const std::string_view>(kRemindersPhrasesEn),
                 std::span<const std::string_view>(kMemoryPhrasesEn), std::span<const std::string_view>(kNonePhrases),
                 std::span<const std::string_view>(kPeoplePhrasesEn), std::span<const std::string_view>(kTasksPhrasesEn),
                 std::span<const std::string_view>(kProjectsPhrasesEn)}},
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
    case ContextFacet::People:
      return "people";
    case ContextFacet::Tasks:
      return "tasks";
    case ContextFacet::Projects:
      return "projects";
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

std::vector<ContextFacet> facetsInText(FacetQuery query)
{
  const FacetTable& table = tableFor(query.lang);
  const std::string folded = text_norm::folded(std::string(query.text));
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
  if (startsWith("task."))
    return ContextFacet::Tasks;
  if (startsWith("project."))
    return ContextFacet::Projects;
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
