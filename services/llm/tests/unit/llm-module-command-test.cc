#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/tools/module-command-lexicon.hxx>
#include <feature/llm/services/tools/module-command.hxx>

#include <json/reader.h>

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
ModuleSnapshot shippedModules()
{
  std::ifstream file(ARGUS_SETTINGS_MODULES_FILE);
  REQUIRE(file.good());
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  REQUIRE(Json::parseFromStream(builder, file, &root, &errors));
  ModuleFlags flags;
  for (const auto& module : root["modules"]) {
    ModuleFlag flag;
    flag.id = module["id"].asString();
    flag.kind = module["kind"].asString();
    flag.name = {.es = module["name"]["es"].asString(), .en = module["name"]["en"].asString()};
    flag.summary = {.es = module["summary"]["es"].asString(), .en = module["summary"]["en"].asString()};
    flags.push_back(std::move(flag));
  }
  return ModuleSnapshot(std::move(flags));
}

std::string routed(const std::string& utterance)
{
  static const ModuleSnapshot modules = shippedModules();
  const auto command = module_command::commandFor({.utterance = utterance, .modules = modules});
  return command ? command->tool : std::string();
}

std::string moduleOf(const std::string& utterance)
{
  static const ModuleSnapshot modules = shippedModules();
  const auto command = module_command::commandFor({.utterance = utterance, .modules = modules});
  return command ? command->arguments.get("module", "").asString() : std::string();
}

struct Case
{
  std::string id;
  std::string group;
  std::string lang;
  std::string variant;
  std::string utterance;
  std::string expected;
  std::string module;
  std::string role;
  bool attemptsInactive{false};
};

bool heldBy(const std::string& role, const std::string& tool)
{
  const bool ownerOnly = tool == "modules.enable" || tool == "modules.disable" || tool == "modules.open_purge_screen";
  if (ownerOnly)
    return role == "owner";
  if (tool == "modules.request")
    return role != "owner";
  return true;
}

std::vector<Case> corpus()
{
  std::ifstream file(ARGUS_EVAL_CASES_FILE);
  REQUIRE(file.good());
  std::vector<Case> cases;
  std::string line;
  while (std::getline(file, line)) {
    Json::Value row;
    std::istringstream input(line);
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, input, &row, &errors))
      continue;
    Case entry;
    entry.id = row["id"].asString();
    entry.group = row["group"].asString();
    entry.lang = row["lang"].asString();
    entry.variant = row["variant"].asString();
    entry.utterance = row["script"][0].asString();
    entry.role = row["role"].asString();
    const Json::Value& expect = row["expect"];
    if (expect["calls"].isArray() && !expect["calls"].empty()) {
      entry.expected = expect["calls"][0]["tool"].asString();
      for (const auto& arg : expect["calls"][0]["args"])
        if (arg["arg"].asString() == "module")
          entry.module = arg["equals"].asString();
    }
    else if (expect.isMember("confirm")) {
      entry.expected = expect["confirm"]["tool"].asString();
      for (const auto& arg : expect["confirm"]["args"])
        if (arg["arg"].asString() == "module")
          entry.module = arg["equals"].asString();
    }
    else if (expect.isMember("inactive")) {
      entry.expected = expect["inactive"]["attempted"].asString();
    }
    else if (expect.isMember("offerAccept")) {
      entry.expected = expect["offerAccept"]["attempted"].asString();
    }
    else if (expect.isMember("offerDecline")) {
      entry.attemptsInactive = true;
    }
    cases.push_back(std::move(entry));
  }
  return cases;
}

bool moduleFamily(const std::string& tool)
{
  return tool.starts_with("calendar.") || tool.starts_with("task.") || tool.starts_with("project.") ||
         tool.starts_with("modules.");
}

struct Tally
{
  int cases{0};
  int hit{0};
  int missed{0};
  int wrong{0};
  int falseRoutes{0};
  std::vector<std::string> misses;
};
}

TEST_CASE("the router picks the tool the corpus expects for the module families and routes nothing else")
{
  std::map<std::string, Tally> byTool;
  Tally negatives;
  std::map<std::string, Tally> bySplit;
  for (const Case& entry : corpus()) {
    const bool expectedFamily = moduleFamily(entry.expected);
    std::string got = routed(entry.utterance);
    if (!heldBy(entry.role, got))
      got.clear();
    const std::string split = (std::stoi(entry.id.substr(entry.id.find_last_of('-') + 1)) % 2 == 0) ? "even" : "odd";
    if (!expectedFamily && entry.attemptsInactive)
      continue;
    if (!expectedFamily) {
      ++negatives.cases;
      if (!got.empty()) {
        ++negatives.falseRoutes;
        negatives.misses.push_back(entry.id + " [" + entry.group + "] '" + entry.utterance + "' -> " + got);
      }
      continue;
    }
    Tally& tally = byTool[entry.expected];
    ++tally.cases;
    Tally& half = bySplit[split];
    ++half.cases;
    if (got == entry.expected && (entry.module.empty() || moduleOf(entry.utterance) == entry.module)) {
      ++tally.hit;
      ++half.hit;
    }
    else if (got.empty()) {
      ++tally.missed;
      ++half.missed;
      tally.misses.push_back(entry.id + " '" + entry.utterance + "' -> nothing");
    }
    else {
      ++tally.wrong;
      ++half.wrong;
      tally.misses.push_back(entry.id + " '" + entry.utterance + "' -> " + got + " " + moduleOf(entry.utterance));
    }
  }
  std::ostringstream report;
  int cases = 0;
  int hits = 0;
  for (const auto& [tool, tally] : byTool) {
    report << tool << ": " << tally.hit << "/" << tally.cases << " missed " << tally.missed << " wrong " << tally.wrong << "\n";
    for (const auto& miss : tally.misses)
      report << "    " << miss << "\n";
    cases += tally.cases;
    hits += tally.hit;
  }
  for (const auto& [split, tally] : bySplit)
    report << "split " << split << ": " << tally.hit << "/" << tally.cases << "\n";
  report << "negatives: " << negatives.falseRoutes << " false routes over " << negatives.cases << "\n";
  for (const auto& miss : negatives.misses)
    report << "    " << miss << "\n";
  report << "total: " << hits << "/" << cases << "\n";
  MESSAGE(report.str());
  CHECK(negatives.falseRoutes == 0);
  CHECK(hits * 100 >= cases * 90);
}

namespace
{
struct Phrase
{
  std::string utterance;
  std::string tool;
};
}

TEST_CASE("fresh paraphrases the corpus does not contain route to the same tools")
{
  const std::vector<Phrase> fresh{
      {.utterance = "programa una reunión con el equipo el lunes a las nueve", .tool = "calendar.create_event"},
      {.utterance = "ponme una cita con el médico mañana a las diez", .tool = "calendar.create_event"},
      {.utterance = "agéndame el dentista para el viernes a las cuatro", .tool = "calendar.create_event"},
      {.utterance = "necesito agendar una reunión con mi jefe el jueves", .tool = "calendar.create_event"},
      {.utterance = "¿puedes agendarme una cita con la doctora el sábado?", .tool = "calendar.create_event"},
      {.utterance = "quiero programar una llamada con mi hermana mañana a las siete", .tool = "calendar.create_event"},
      {.utterance = "haz una reunión con Pedro el lunes a las once", .tool = "calendar.create_event"},
      {.utterance = "añade a mi calendario cena con los suegros el domingo", .tool = "calendar.create_event"},
      {.utterance = "mete en la agenda la cita del banco el miércoles", .tool = "calendar.create_event"},
      {.utterance = "schedule lunch with Maria on friday at noon", .tool = "calendar.create_event"},
      {.utterance = "can you book a dentist appointment for next tuesday at three", .tool = "calendar.create_event"},
      {.utterance = "add a meeting with the team on monday at nine to my calendar", .tool = "calendar.create_event"},
      {.utterance = "set up a call with my boss tomorrow at five", .tool = "calendar.create_event"},
      {.utterance = "put dinner with the Garcias on my calendar for saturday", .tool = "calendar.create_event"},
      {.utterance = "dime cómo está mi agenda hoy", .tool = "calendar.list_events"},
      {.utterance = "¿tengo reuniones mañana?", .tool = "calendar.list_events"},
      {.utterance = "¿cuándo es mi siguiente reunión?", .tool = "calendar.list_events"},
      {.utterance = "léeme mi agenda de la semana", .tool = "calendar.list_events"},
      {.utterance = "do I have any meetings tomorrow", .tool = "calendar.list_events"},
      {.utterance = "show me my appointments for friday", .tool = "calendar.list_events"},
      {.utterance = "read me my agenda", .tool = "calendar.list_events"},
      {.utterance = "cancela la cita con el dentista", .tool = "calendar.cancel_event"},
      {.utterance = "quita la reunión del jueves de mi agenda", .tool = "calendar.cancel_event"},
      {.utterance = "ya no voy a ir a la cita, cancélala", .tool = "calendar.cancel_event"},
      {.utterance = "cancel my appointment on friday", .tool = "calendar.cancel_event"},
      {.utterance = "delete the meeting with John", .tool = "calendar.cancel_event"},
      {.utterance = "apúntame como pendiente llamar al banco", .tool = "task.create"},
      {.utterance = "crea una tarea: sacar la basura", .tool = "task.create"},
      {.utterance = "nueva tarea: pagar el agua", .tool = "task.create"},
      {.utterance = "add buy milk to my task list", .tool = "task.create"},
      {.utterance = "create a to do for the taxes", .tool = "task.create"},
      {.utterance = "dime mis tareas", .tool = "task.list"},
      {.utterance = "show me my tasks for today", .tool = "task.list"},
      {.utterance = "qué tareas tengo pendientes esta semana", .tool = "task.list"},
      {.utterance = "ya hice la tarea de lavar el carro, márcala como hecha", .tool = "task.complete"},
      {.utterance = "termina la tarea de llamar al banco", .tool = "task.complete"},
      {.utterance = "mark the laundry task done", .tool = "task.complete"},
      {.utterance = "crea un proyecto para la boda", .tool = "project.create"},
      {.utterance = "inicia un proyecto de remodelación", .tool = "project.create"},
      {.utterance = "open a project called garden", .tool = "project.create"},
      {.utterance = "muéstrame mis proyectos", .tool = "project.list"},
      {.utterance = "qué proyectos tengo en marcha", .tool = "project.list"},
      {.utterance = "qué módulos puedo activar", .tool = "modules.list"},
      {.utterance = "enséñame los módulos", .tool = "modules.list"},
      {.utterance = "activa productividad", .tool = "modules.enable"},
      {.utterance = "quiero activar la vigilancia", .tool = "modules.enable"},
      {.utterance = "habilítame la productividad", .tool = "modules.enable"},
      {.utterance = "turn on surveillance", .tool = "modules.enable"},
      {.utterance = "ya no quiero la productividad, apágala", .tool = "modules.disable"},
      {.utterance = "desactívame la vigilancia", .tool = "modules.disable"},
      {.utterance = "disable the productivity module", .tool = "modules.disable"},
      {.utterance = "cómo funciona la productividad", .tool = "modules.explain"},
      {.utterance = "tell me about the surveillance module", .tool = "modules.explain"},
      {.utterance = "how does the productivity module work", .tool = "modules.explain"},
      {.utterance = "borra los datos de la vigilancia", .tool = "modules.open_purge_screen"},
      {.utterance = "delete all the data from surveillance", .tool = "modules.open_purge_screen"},
      {.utterance = "pídele al dueño que active la vigilancia", .tool = "modules.request"},
  };
  std::vector<std::string> failures;
  for (const Phrase& phrase : fresh)
    if (routed(phrase.utterance) != phrase.tool)
      failures.push_back("'" + phrase.utterance + "' wanted " + phrase.tool + " got '" + routed(phrase.utterance) + "'");
  std::ostringstream report;
  report << (fresh.size() - failures.size()) << "/" << fresh.size() << " fresh paraphrases routed\n";
  for (const auto& failure : failures)
    report << "    " << failure << "\n";
  MESSAGE(report.str());
  CHECK(failures.empty());
}

TEST_CASE("near misses and everyday talk route nowhere")
{
  const std::vector<std::string> quiet{
      "recuérdame llamar al plomero mañana a las nueve",
      "mañana tengo una reunión con Pedro a las diez",
      "anota que mi reunión con Pedro es el lunes",
      "mi cita con el dentista es el martes, recuérdalo",
      "la tarea de mi hijo es muy difícil",
      "qué hora es",
      "pon música suave",
      "apaga la luz de la cocina",
      "apaga el televisor",
      "desactiva el modo noche",
      "activa la alarma",
      "borra todo lo que sabes de mi mamá",
      "cuántos proyectos de ley aprobó el congreso",
      "cuéntame un chiste",
      "qué es un módulo lunar",
      "when is my dad's birthday",
      "what time does the store open",
      "I have a meeting tomorrow so I will be late",
      "remind me to call the dentist on friday",
      "turn off the lights",
      "play some music",
      "tell me a story",
      "dime una receta de arroz con pollo",
      "agenda telefónica de la oficina",
      "el calendario maya tenía 365 días",
      "gracias argus",
      "sí",
      "no, déjalo",
  };
  std::vector<std::string> routedNow;
  for (const auto& utterance : quiet)
    if (!routed(utterance).empty())
      routedNow.push_back("'" + utterance + "' -> " + routed(utterance));
  std::ostringstream report;
  for (const auto& line : routedNow)
    report << "    " << line << "\n";
  MESSAGE(report.str());
  CHECK(routedNow.empty());
}

TEST_CASE("every phrase group of both languages is filled, and nothing is spelled with accents or capitals")
{
  for (const module_command::Table& table : module_command::tables()) {
    for (std::size_t group = 0; group < module_command::kGroupCount; ++group) {
      const bool noEnglishAgendaVerb = table.language == "en" && group == static_cast<std::size_t>(module_command::Group::LeadAgenda);
      INFO(table.language << " group " << group);
      if (!noEnglishAgendaVerb)
        CHECK_FALSE(table.groups[group].empty());
      for (const std::string_view phrase : table.groups[group])
        for (const char c : phrase)
          CHECK(((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' '));
    }
  }
}

TEST_CASE("a second batch of new phrasings, written after the rules were fixed, routes the same way")
{
  const std::vector<Phrase> fresh{
      {.utterance = "oye argus anótame una reunión con los proveedores el martes a las ocho", .tool = "calendar.create_event"},
      {.utterance = "pon una cita con la peluquera el sábado a las once", .tool = "calendar.create_event"},
      {.utterance = "por favor agenda una videollamada con el contador para mañana a las cinco", .tool = "calendar.create_event"},
      {.utterance = "me gustaría reservar una reunión con el director el miércoles", .tool = "calendar.create_event"},
      {.utterance = "añade un evento para el cumpleaños de Lucía el diez de mayo", .tool = "calendar.create_event"},
      {.utterance = "can you schedule a call with my accountant for thursday at four", .tool = "calendar.create_event"},
      {.utterance = "please add a doctor appointment to my calendar on monday at eight", .tool = "calendar.create_event"},
      {.utterance = "I need to schedule an interview with Sara on friday at ten", .tool = "calendar.create_event"},
      {.utterance = "put the school meeting in my agenda for next wednesday at six pm", .tool = "calendar.create_event"},
      {.utterance = "qué reuniones tengo hoy", .tool = "calendar.list_events"},
      {.utterance = "¿cuándo tengo la próxima cita con el dentista?", .tool = "calendar.list_events"},
      {.utterance = "dime si tengo algo agendado el jueves", .tool = "calendar.list_events"},
      {.utterance = "muéstrame mi calendario de la próxima semana", .tool = "calendar.list_events"},
      {.utterance = "what meetings do I have today", .tool = "calendar.list_events"},
      {.utterance = "what's on my agenda for friday", .tool = "calendar.list_events"},
      {.utterance = "am I busy on thursday", .tool = "calendar.list_events"},
      {.utterance = "list my appointments for the week", .tool = "calendar.list_events"},
      {.utterance = "agrega una tarea para renovar el pasaporte", .tool = "task.create"},
      {.utterance = "añade pendiente: revisar el extintor", .tool = "task.create"},
      {.utterance = "apunta una tarea nueva: cambiar los focos", .tool = "task.create"},
      {.utterance = "crea un to do para pagar la luz", .tool = "task.create"},
      {.utterance = "please create a task for renewing my passport", .tool = "task.create"},
      {.utterance = "add a new task: fix the gate", .tool = "task.create"},
      {.utterance = "add call the bank to my to-do list", .tool = "task.create"},
      {.utterance = "qué pendientes tengo hoy", .tool = "task.list"},
      {.utterance = "give me my task list", .tool = "task.list"},
      {.utterance = "which tasks are open", .tool = "task.list"},
      {.utterance = "lista las tareas del proyecto cocina", .tool = "task.list"},
      {.utterance = "tacha lo de comprar el pan", .tool = "task.complete"},
      {.utterance = "completa la tarea de renovar el pasaporte", .tool = "task.complete"},
      {.utterance = "marca la tarea del pasaporte como terminada", .tool = "task.complete"},
      {.utterance = "I finished the passport task, mark it as done", .tool = "task.complete"},
      {.utterance = "check off the taxes task", .tool = "task.complete"},
      {.utterance = "empieza un proyecto para el viaje a Cusco", .tool = "project.create"},
      {.utterance = "necesito un proyecto para organizar la boda", .tool = "project.create"},
      {.utterance = "let's start a project for the garage", .tool = "project.create"},
      {.utterance = "create a project named wedding", .tool = "project.create"},
      {.utterance = "cuántos proyectos tengo", .tool = "project.list"},
      {.utterance = "qué proyectos hay abiertos", .tool = "project.list"},
      {.utterance = "show my projects", .tool = "project.list"},
      {.utterance = "qué módulos hay disponibles", .tool = "modules.list"},
      {.utterance = "which modules can I turn on", .tool = "modules.list"},
      {.utterance = "enciende el módulo de productividad", .tool = "modules.enable"},
      {.utterance = "apaga el módulo de vigilancia", .tool = "modules.disable"},
      {.utterance = "turn on the productivity module", .tool = "modules.enable"},
      {.utterance = "para qué sirve la productividad", .tool = "modules.explain"},
      {.utterance = "what does the surveillance module do", .tool = "modules.explain"},
      {.utterance = "borra todos los datos de productividad", .tool = "modules.open_purge_screen"},
      {.utterance = "ask the owner to turn on surveillance", .tool = "modules.request"},
      {.utterance = "dile al dueño que prenda la vigilancia", .tool = "modules.request"},
      {.utterance = "cancela la reunión con los proveedores", .tool = "calendar.cancel_event"},
      {.utterance = "borra la cita del viernes del calendario", .tool = "calendar.cancel_event"},
  };
  std::vector<std::string> failures;
  for (const Phrase& phrase : fresh)
    if (routed(phrase.utterance) != phrase.tool)
      failures.push_back("'" + phrase.utterance + "' wanted " + phrase.tool + " got '" + routed(phrase.utterance) + "'");
  std::ostringstream report;
  report << (fresh.size() - failures.size()) << "/" << fresh.size() << " batch-2 phrasings routed\n";
  for (const auto& failure : failures)
    report << "    " << failure << "\n";
  MESSAGE(report.str());
  CHECK(failures.empty());
}

TEST_CASE("a second batch of near misses routes nowhere")
{
  const std::vector<std::string> quiet{
      "tengo que sacar a pasear al perro mañana",
      "la reunión de ayer fue larga",
      "mi proyecto de ciencias salió bien",
      "no encuentro mi agenda de papel",
      "qué módulo lunar usó la NASA",
      "cuéntame cómo funciona un motor",
      "activa el altavoz",
      "elimina el ruido de fondo",
      "crea una lista de compras",
      "añade leche a la lista del supermercado",
      "programa la lavadora a las ocho",
      "reserva una mesa para dos a las nueve",
      "book a table for two at nine",
      "reserva una habitación en el hotel para el viernes",
      "set up the router",
      "set the alarm for seven",
      "put the groceries on the table",
      "add sugar to my coffee",
      "cancela la suscripción de la revista",
      "delete the file",
      "what is the weather tomorrow",
      "what's on tv tonight",
      "tell me about the history of Rome",
      "turn it off",
      "apágalo",
      "cuántos días faltan para navidad",
      "dime qué hora es en Tokio",
      "léeme las noticias",
      "show me a picture of a cat",
      "list the planets of the solar system",
  };
  std::vector<std::string> routedNow;
  for (const auto& utterance : quiet)
    if (!routed(utterance).empty())
      routedNow.push_back("'" + utterance + "' -> " + routed(utterance));
  std::ostringstream report;
  report << routedNow.size() << " false routes over " << quiet.size() << "\n";
  for (const auto& line : routedNow)
    report << "    " << line << "\n";
  MESSAGE(report.str());
  CHECK(routedNow.empty());
}
