#include "speech-cues.hxx"

#include <array>
#include <string_view>

namespace turn::speech
{

namespace
{

constexpr std::array<std::string_view, 3> kTitleEs{"como se llama", "como lo llamo", "titulo"};
constexpr std::array<std::string_view, 5> kTitleEn{"what is it called",
                                                  "what's it called",
                                                  "what should i call",
                                                  "what should i call it",
                                                  "title"};
constexpr std::array<std::string_view, 3> kNameEs{"como se llama", "nombre", "como lo llamo"};
constexpr std::array<std::string_view, 2> kNameEn{"what is it called", "name"};
constexpr std::array<std::string_view, 3> kTextEs{"que guardo", "que anoto", "que recuerdo"};
constexpr std::array<std::string_view, 3> kTextEn{"what should i save", "what do i note", "what do i remember"};
constexpr std::array<std::string_view, 3> kQueryEs{"que busco", "que quieres que busque", "de que"};
constexpr std::array<std::string_view, 2> kQueryEn{"what should i look for", "which one"};
constexpr std::array<std::string_view, 3> kSubjectEs{"quien", "de quien", "con quien"};
constexpr std::array<std::string_view, 3> kSubjectEn{"who", "whose", "with whom"};
constexpr std::array<std::string_view, 3> kWhenEs{"cuando", "que dia", "a que hora"};
constexpr std::array<std::string_view, 3> kWhenEn{"when", "what day", "what time"};
constexpr std::array<std::string_view, 4> kStartsAtEs{"cuando", "que dia", "a que hora", "fecha"};
constexpr std::array<std::string_view, 4> kStartsAtEn{"when", "what day", "what time", "date"};
constexpr std::array<std::string_view, 3> kDueAtEs{"para cuando", "cuando vence", "fecha limite"};
constexpr std::array<std::string_view, 3> kDueAtEn{"when is it due", "when does it fall due", "deadline"};
constexpr std::array<std::string_view, 3> kRangeEs{"desde cuando", "hasta cuando", "de que dia a que dia"};
constexpr std::array<std::string_view, 3> kRangeEn{"from when", "until when", "which days"};
constexpr std::array<std::string_view, 2> kScreenEs{"que pantalla", "a que pantalla"};
constexpr std::array<std::string_view, 2> kScreenEn{"which screen", "what screen"};
constexpr std::array<std::string_view, 2> kModuleEs{"que modulo", "de que modulo"};
constexpr std::array<std::string_view, 2> kModuleEn{"which module", "what module"};
constexpr std::array<std::string_view, 2> kModeEs{"que modo", "como lo pongo"};
constexpr std::array<std::string_view, 2> kModeEn{"which mode", "what mode"};
constexpr std::array<std::string_view, 3> kEnvironmentEs{"que lugar", "en que ambiente", "donde"};
constexpr std::array<std::string_view, 3> kEnvironmentEn{"which place", "what place", "where"};
constexpr std::array<std::string_view, 2> kCameraEs{"que camara", "cual"};
constexpr std::array<std::string_view, 2> kCameraEn{"which camera", "what camera"};
constexpr std::array<std::string_view, 2> kLocationEs{"donde", "en que lugar"};
constexpr std::array<std::string_view, 2> kLocationEn{"where", "in which place"};
constexpr std::array<std::string_view, 3> kProjectEs{"proyecto", "en cual", "para cual"};
constexpr std::array<std::string_view, 3> kProjectEn{"project", "which project", "for which"};
constexpr std::array<std::string_view, 2> kEventEs{"que evento", "cual"};
constexpr std::array<std::string_view, 2> kEventEn{"which event", "which one"};
constexpr std::array<std::string_view, 2> kTaskEs{"que tarea", "cual"};
constexpr std::array<std::string_view, 2> kTaskEn{"which task", "which one"};

constexpr std::array<CueRow, 38> kRows{{
    {.slot = "title", .language = "es", .cues = kTitleEs},
    {.slot = "title", .language = "en", .cues = kTitleEn},
    {.slot = "name", .language = "es", .cues = kNameEs},
    {.slot = "name", .language = "en", .cues = kNameEn},
    {.slot = "text", .language = "es", .cues = kTextEs},
    {.slot = "text", .language = "en", .cues = kTextEn},
    {.slot = "query", .language = "es", .cues = kQueryEs},
    {.slot = "query", .language = "en", .cues = kQueryEn},
    {.slot = "subject", .language = "es", .cues = kSubjectEs},
    {.slot = "subject", .language = "en", .cues = kSubjectEn},
    {.slot = "when", .language = "es", .cues = kWhenEs},
    {.slot = "when", .language = "en", .cues = kWhenEn},
    {.slot = "starts_at", .language = "es", .cues = kStartsAtEs},
    {.slot = "starts_at", .language = "en", .cues = kStartsAtEn},
    {.slot = "due_at", .language = "es", .cues = kDueAtEs},
    {.slot = "due_at", .language = "en", .cues = kDueAtEn},
    {.slot = "from", .language = "es", .cues = kRangeEs},
    {.slot = "from", .language = "en", .cues = kRangeEn},
    {.slot = "to", .language = "es", .cues = kRangeEs},
    {.slot = "to", .language = "en", .cues = kRangeEn},
    {.slot = "screen", .language = "es", .cues = kScreenEs},
    {.slot = "screen", .language = "en", .cues = kScreenEn},
    {.slot = "module", .language = "es", .cues = kModuleEs},
    {.slot = "module", .language = "en", .cues = kModuleEn},
    {.slot = "mode", .language = "es", .cues = kModeEs},
    {.slot = "mode", .language = "en", .cues = kModeEn},
    {.slot = "environment", .language = "es", .cues = kEnvironmentEs},
    {.slot = "environment", .language = "en", .cues = kEnvironmentEn},
    {.slot = "camera", .language = "es", .cues = kCameraEs},
    {.slot = "camera", .language = "en", .cues = kCameraEn},
    {.slot = "location", .language = "es", .cues = kLocationEs},
    {.slot = "location", .language = "en", .cues = kLocationEn},
    {.slot = "project", .language = "es", .cues = kProjectEs},
    {.slot = "project", .language = "en", .cues = kProjectEn},
    {.slot = "event_id", .language = "es", .cues = kEventEs},
    {.slot = "event_id", .language = "en", .cues = kEventEn},
    {.slot = "task_id", .language = "es", .cues = kTaskEs},
    {.slot = "task_id", .language = "en", .cues = kTaskEn},
}};

constexpr std::array<std::string_view, 0> kNoCues{};

struct ActionRow
{
  std::string_view tool;
  std::string_view labelEs;
  std::string_view labelEn;
  std::span<const std::string_view> markersEs;
  std::span<const std::string_view> markersEn;
};

constexpr std::array<std::string_view, 5> kCreateEventEs{"agend", "crear", "creo", "program", "anot"};
constexpr std::array<std::string_view, 5> kCreateEventEn{"agend", "schedul", "creat", "add", "book"};
constexpr std::array<std::string_view, 4> kListEventsEs{"agenda", "revis", "miro", "consult"};
constexpr std::array<std::string_view, 4> kListEventsEn{"agenda", "check", "look", "see"};
constexpr std::array<std::string_view, 4> kCancelEventEs{"cancel", "borr", "quit", "elimin"};
constexpr std::array<std::string_view, 4> kCancelEventEn{"cancel", "delet", "remov", "drop"};
constexpr std::array<std::string_view, 4> kAddTaskEs{"anot", "apunt", "tarea", "crear"};
constexpr std::array<std::string_view, 4> kAddTaskEn{"add", "note", "task", "creat"};
constexpr std::array<std::string_view, 1> kTasksEs{"tarea"};
constexpr std::array<std::string_view, 1> kTasksEn{"task"};
constexpr std::array<std::string_view, 4> kCompleteTaskEs{"hech", "complet", "marc", "termin"};
constexpr std::array<std::string_view, 4> kCompleteTaskEn{"done", "complet", "mark", "finish"};
constexpr std::array<std::string_view, 2> kCreateProjectEs{"proyecto", "crear"};
constexpr std::array<std::string_view, 2> kCreateProjectEn{"project", "creat"};
constexpr std::array<std::string_view, 1> kProjectsEs{"proyecto"};
constexpr std::array<std::string_view, 1> kProjectsEn{"project"};
constexpr std::array<std::string_view, 1> kModulesEs{"modulo"};
constexpr std::array<std::string_view, 1> kModulesEn{"module"};
constexpr std::array<std::string_view, 4> kRememberEs{"recuerd", "guard", "memoria", "aprend"};
constexpr std::array<std::string_view, 5> kRememberEn{"rememb", "memor", "save", "store", "keep"};
constexpr std::array<std::string_view, 3> kRecallEs{"busc", "recuerd", "sab"};
constexpr std::array<std::string_view, 4> kRecallEn{"look", "rememb", "know", "search"};
constexpr std::array<std::string_view, 3> kRemindEs{"record", "avis", "recuerd"};
constexpr std::array<std::string_view, 3> kRemindEn{"remind", "rememb", "alert"};
constexpr std::array<std::string_view, 1> kForgetEs{"olvid"};
constexpr std::array<std::string_view, 1> kForgetEn{"forget"};
constexpr std::array<std::string_view, 4> kShowCameraEs{"camara", "mostr", "ver", "ensen"};
constexpr std::array<std::string_view, 4> kShowCameraEn{"camera", "show", "view", "see"};
constexpr std::array<std::string_view, 3> kGuardModeEs{"vigilan", "modo", "guardia"};
constexpr std::array<std::string_view, 3> kGuardModeEn{"guard", "mode", "vigilan"};

constexpr std::array<ActionRow, 20> kActions{{
    {.tool = "calendar.create_event",
     .labelEs = "que lo agende",
     .labelEn = "schedule it",
     .markersEs = kCreateEventEs,
     .markersEn = kCreateEventEn},
    {.tool = "calendar.list_events",
     .labelEs = "que revise tu agenda",
     .labelEn = "check your agenda",
     .markersEs = kListEventsEs,
     .markersEn = kListEventsEn},
    {.tool = "calendar.cancel_event",
     .labelEs = "que cancele un evento",
     .labelEn = "cancel an event",
     .markersEs = kCancelEventEs,
     .markersEn = kCancelEventEn},
    {.tool = "task.create",
     .labelEs = "que lo anote como tarea",
     .labelEn = "add it as a task",
     .markersEs = kAddTaskEs,
     .markersEn = kAddTaskEn},
    {.tool = "task.list",
     .labelEs = "que te diga tus tareas",
     .labelEn = "read you your tasks",
     .markersEs = kTasksEs,
     .markersEn = kTasksEn},
    {.tool = "task.complete",
     .labelEs = "que marque una tarea como hecha",
     .labelEn = "mark a task as done",
     .markersEs = kCompleteTaskEs,
     .markersEn = kCompleteTaskEn},
    {.tool = "project.create",
     .labelEs = "que cree un proyecto",
     .labelEn = "create a project",
     .markersEs = kCreateProjectEs,
     .markersEn = kCreateProjectEn},
    {.tool = "project.list",
     .labelEs = "que te diga tus proyectos",
     .labelEn = "list your projects",
     .markersEs = kProjectsEs,
     .markersEn = kProjectsEn},
    {.tool = "modules.list",
     .labelEs = "que te diga que modulos hay",
     .labelEn = "tell you which modules there are",
     .markersEs = kModulesEs,
     .markersEn = kModulesEn},
    {.tool = "modules.explain",
     .labelEs = "que te explique un modulo",
     .labelEn = "explain a module",
     .markersEs = kModulesEs,
     .markersEn = kModulesEn},
    {.tool = "modules.enable",
     .labelEs = "que active un modulo",
     .labelEn = "turn a module on",
     .markersEs = kModulesEs,
     .markersEn = kModulesEn},
    {.tool = "modules.disable",
     .labelEs = "que apague un modulo",
     .labelEn = "turn a module off",
     .markersEs = kModulesEs,
     .markersEn = kModulesEn},
    {.tool = "modules.request",
     .labelEs = "que se lo pida al dueno",
     .labelEn = "ask the owner",
     .markersEs = kModulesEs,
     .markersEn = kModulesEn},
    {.tool = "modules.open_purge_screen",
     .labelEs = "que abra la pantalla de datos",
     .labelEn = "open the data screen",
     .markersEs = kModulesEs,
     .markersEn = kModulesEn},
    {.tool = "memory.remember",
     .labelEs = "que lo recuerde",
     .labelEn = "remember it",
     .markersEs = kRememberEs,
     .markersEn = kRememberEn},
    {.tool = "memory.recall",
     .labelEs = "que lo busque en lo que se",
     .labelEn = "look it up in what i know",
     .markersEs = kRecallEs,
     .markersEn = kRecallEn},
    {.tool = "memory.remind",
     .labelEs = "que te lo recuerde",
     .labelEn = "remind you",
     .markersEs = kRemindEs,
     .markersEn = kRemindEn},
    {.tool = "memory.forget",
     .labelEs = "que lo olvide",
     .labelEn = "forget it",
     .markersEs = kForgetEs,
     .markersEn = kForgetEn},
    {.tool = "app.show_camera",
     .labelEs = "que te muestre una camara",
     .labelEn = "show you a camera",
     .markersEs = kShowCameraEs,
     .markersEn = kShowCameraEn},
    {.tool = "app.set_guard_mode",
     .labelEs = "que cambie la vigilancia",
     .labelEn = "change the guard mode",
     .markersEs = kGuardModeEs,
     .markersEn = kGuardModeEn},
}};

const ActionRow* actionRow(std::string_view tool)
{
  for (const ActionRow& row : kActions)
    if (row.tool == tool)
      return &row;
  return nullptr;
}

}

std::span<const CueRow> cueRows()
{
  return kRows;
}

std::span<const std::string_view> cuesFor(const CueQuery& query)
{
  const std::string_view language = query.lang == "en" ? std::string_view("en") : std::string_view("es");
  for (const CueRow& row : kRows)
    if (row.slot == query.slot && row.language == language)
      return row.cues;
  return kNoCues;
}

std::span<const std::string_view> actionMarkers(const ActionQuery& query)
{
  const ActionRow* row = actionRow(query.tool);
  if (row == nullptr)
    return kNoCues;
  return query.lang == "en" ? row->markersEn : row->markersEs;
}

std::string_view slotActionName(const ActionQuery& query)
{
  const ActionRow* row = actionRow(query.tool);
  if (row == nullptr)
    return query.lang == "en" ? std::string_view("do it") : std::string_view("que lo haga");
  return query.lang == "en" ? row->labelEn : row->labelEs;
}

}
