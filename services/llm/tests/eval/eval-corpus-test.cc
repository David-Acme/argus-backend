#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "call-faithfulness-checks.hxx"
#include "eval-case.hxx"
#include "eval-report.hxx"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{

const std::vector<eval::EvalCase>& corpus()
{
  static const eval::LoadedCases loaded = eval::loadCases(ARGUS_EVAL_CASES);
  REQUIRE_MESSAGE(loaded.error.empty(), loaded.error);
  return loaded.cases;
}

bool contains(const std::vector<std::string>& values, const std::string& wanted)
{
  return std::ranges::find(values, wanted) != values.end();
}

std::string joined(std::initializer_list<std::string_view> parts)
{
  std::string out;
  for (std::string_view part : parts)
    out += part;
  return out;
}

}

TEST_CASE("every judge case is well formed and unique")
{
  const std::set<std::string> routes{"memory_save", "memory_recall", "reminder_set", "memory_forget", "camera", "none"};
  const std::set<std::string> variants{"neutral", "pe", "stt", "en"};
  const std::set<std::string> roles{"owner", "resident", "guard", "guest"};
  std::set<std::string> ids;
  CHECK(corpus().size() >= 700);
  for (const auto& item : corpus()) {
    CHECK_MESSAGE(ids.insert(item.id).second, "duplicate id " << item.id);
    CHECK_MESSAGE(routes.contains(item.route), item.id << " route " << item.route);
    CHECK_MESSAGE(variants.contains(item.variant), item.id << " variant " << item.variant);
    CHECK_MESSAGE(roles.contains(item.role), item.id << " role " << item.role);
    CHECK_MESSAGE((item.lang == "es" || item.lang == "en"), item.id << " lang " << item.lang);
    CHECK_MESSAGE(!item.script.empty(), item.id << " has no utterance");
    const int judged = (item.calls.empty() ? 0 : 1) + (item.inactive ? 1 : 0) + (item.confirm ? 1 : 0) +
                       (item.offerAccept ? 1 : 0) + (item.offerDecline ? 1 : 0);
    CHECK_MESSAGE(judged <= 1, item.id << " carries " << judged << " kinds of expectation");
  }
}

TEST_CASE("agenda, calendar, task and project utterances are labelled none in every variant")
{
  std::map<std::string, int> perVariant;
  for (const auto& item : corpus()) {
    if (item.group != "productivity")
      continue;
    CHECK_MESSAGE(item.route == "none", item.id << " routes to " << item.route);
    ++perVariant[item.variant];
  }
  CHECK(perVariant["neutral"] >= 30);
  CHECK(perVariant["pe"] >= 10);
  CHECK(perVariant["stt"] >= 8);
  CHECK(perVariant["en"] >= 20);
}

TEST_CASE("every productivity request is asked again with the module off")
{
  std::map<std::string, const eval::EvalCase*> byId;
  for (const auto& item : corpus())
    byId[item.id] = &item;
  int owners = 0;
  int members = 0;
  for (const auto& item : corpus()) {
    if (item.group != "productivity" || item.calls.empty())
      continue;
    const auto twin = std::ranges::find_if(corpus(), [&item](const eval::EvalCase& other) {
      return other.twin == item.id && other.role == "owner";
    });
    REQUIRE_MESSAGE(twin != corpus().end(), item.id << " has no owner twin with productivity off");
    CHECK_FALSE(contains(twin->modules, "productivity"));
    CHECK(twin->utterance() == item.utterance());
    if (!twin->inactive) {
      FAIL_CHECK(item.id << " twin does not expect the module-off answer");
      continue;
    }
    CHECK(twin->inactive->module == "productivity");
    CHECK(twin->inactive->attempted == item.calls.front().tool);
    CHECK(twin->inactive->audience == "owner");
    CHECK(twin->calls.empty());
    ++owners;
  }
  for (const auto& item : corpus()) {
    if (!item.inactive || item.role == "owner")
      continue;
    CHECK(item.inactive.value().audience == "member");
    ++members;
  }
  CHECK(owners >= 90);
  CHECK(members >= 20);
}

TEST_CASE("a destructive tool is judged both before and after the spoken yes")
{
  std::map<std::string, std::set<std::string>> phases;
  for (const auto& item : corpus()) {
    if (!item.confirm)
      continue;
    phases[item.confirm.value().tool].insert(item.confirm.value().phase);
    if (item.confirm.value().phase == "execute")
      CHECK_MESSAGE(item.script.size() == 2, item.id << " needs the request and the yes");
  }
  for (const char* tool : {"calendar.cancel_event", "modules.disable"}) {
    CHECK(phases[tool].contains("ask"));
    CHECK(phases[tool].contains("execute"));
  }
}

TEST_CASE("the Wilson bounds and the gates behave as documented")
{
  const eval::Interval half = eval::wilson({.hits = 5, .total = 10});
  CHECK(half.lower == doctest::Approx(0.2366).epsilon(0.01));
  CHECK(half.upper == doctest::Approx(0.7634).epsilon(0.01));
  CHECK(eval::wilson({.hits = 0, .total = 0}).upper == 0.0);

  const std::vector<eval::Gate> gates{{.metric = "a", .min = 0.9, .max = std::nullopt},
                                      {.metric = "b", .min = std::nullopt, .max = 0.1},
                                      {.metric = "missing", .min = 0.0, .max = std::nullopt}};
  const eval::Verdict verdict = eval::check(gates, {{"a", 0.8}, {"b", 0.2}});
  CHECK(verdict.failures.size() == 3);
  CHECK(eval::check(gates, {{"a", 0.9}, {"b", 0.1}, {"missing", 1.0}}).passed());
}

TEST_CASE("the roles line leads the static prompt and a per-case override wins")
{
  const std::string prompt = "Eres Argus.";
  const std::string known = "Lo que sabes ahora mismo por la app (menciónala solo si viene al caso):";
  const std::string person = "Lo que sabes de la persona (úsalo con naturalidad, no lo recites):\n- Se llama David.";
  const std::vector<std::string> notes{"Agenda de hoy: libre."};
  const std::string roles = "Hablas con David, el propietario de la casa.";
  const std::optional<std::string> perCase = std::string("Todavía no sabes cómo se llama; pregúntaselo una vez.");
  const std::optional<std::string> suppressed = std::string();

  const call_checks::StaticPromptInput flagged{
      .roles = std::nullopt, .rolesDefault = roles, .prompt = prompt, .person = {}, .known = known, .facts = notes};
  const call_checks::StaticPromptInput overridden{
      .roles = perCase, .rolesDefault = roles, .prompt = prompt, .person = {}, .known = known, .facts = notes};
  const call_checks::StaticPromptInput silenced{
      .roles = suppressed, .rolesDefault = roles, .prompt = prompt, .person = {}, .known = known, .facts = notes};
  const call_checks::StaticPromptInput plain{
      .roles = std::nullopt, .rolesDefault = {}, .prompt = prompt, .person = person, .known = known, .facts = notes};

  CHECK(call_checks::staticPrompt(flagged) ==
        joined({roles, "\n\n", prompt, "\n", known, "\n", "Agenda de hoy: libre."}));
  CHECK(call_checks::staticPrompt(overridden) ==
        joined({*perCase, "\n\n", prompt, "\n", known, "\n", "Agenda de hoy: libre."}));
  CHECK(call_checks::staticPrompt(silenced) == joined({prompt, "\n", known, "\n", "Agenda de hoy: libre."}));
  CHECK(call_checks::staticPrompt(plain) ==
        joined({prompt, "\n\n", person, "\n", known, "\n", "Agenda de hoy: libre."}));
}

TEST_CASE("an interjection-only opening does not count as a sentence")
{
  CHECK(call_checks::sentenceCount("¡Hola! Estoy bien, gracias. ¿En qué puedo ayudarte hoy?") == 2);
  CHECK(call_checks::sentenceCount("Hello! I'm here and doing well, thanks for asking. How about you?") == 2);
  CHECK(call_checks::sentenceCount("El clima está bien. Mañana lloverá. Deberías llevar paraguas.") == 3);
  CHECK(call_checks::sentenceCount("Gracias por avisar. Te cuento algo.") == 1);
  CHECK(call_checks::sentenceCount("¡Hola!") == 0);
}

TEST_CASE("an echoed example reply is caught by containment or by token overlap")
{
  const std::vector<std::string> examples{"¡Hola! Muy bien, gracias por preguntar. ¿Qué tal va tu día?"};
  const std::vector<std::string> none;
  CHECK(call_checks::parrots("¡Hola! Muy bien, gracias por preguntar. ¿Qué tal va tu día?", examples));
  CHECK(call_checks::parrots("Hola muy bien gracias por preguntar que tal va tu dia", examples));
  CHECK_FALSE(call_checks::parrots("¡Hola! Muy bien, ¿y tú?", examples));
  CHECK_FALSE(call_checks::parrots("Te cuento algo interesante sobre los gatos.", examples));
  CHECK_FALSE(call_checks::parrots("¡Hola! Muy bien, gracias por preguntar. ¿Qué tal va tu día?", none));
}

TEST_CASE("a case that expects tokens flags a reply that names none of them")
{
  const std::vector<std::string> expected{"pendiente", "agenda", "nada", "libre"};
  const std::vector<std::string> none;
  CHECK_FALSE(call_checks::missesExpectedToken("Hoy tienes la agenda libre.", expected));
  CHECK_FALSE(call_checks::missesExpectedToken("No hay nada pendiente.", expected));
  CHECK(call_checks::missesExpectedToken("No lo sé.", expected));
  CHECK(call_checks::missesExpectedToken("Vamos a nadar mañana.", expected));
  CHECK_FALSE(call_checks::missesExpectedToken("No lo sé.", none));
}

TEST_CASE("a reply that opens with a greeting is caught when the user did not greet")
{
  CHECK(call_checks::opensWithGreeting("¡Hola! Estoy bien, gracias."));
  CHECK(call_checks::opensWithGreeting("Hello, how are you?"));
  CHECK(call_checks::opensWithGreeting("Buenas noches."));
  CHECK_FALSE(call_checks::opensWithGreeting("La cámara de la cocina está en línea."));
  CHECK_FALSE(call_checks::opensWithGreeting("Gracias por avisar."));
  CHECK_FALSE(call_checks::opensWithGreeting(""));
}

TEST_CASE("a marker that is also an ordinary word counts only in its slang shape")
{
  const std::vector<std::string> markers{"pata", "causa", "al toque"};
  CHECK(call_checks::speaksRegionalism("Gracias, pata, nos vemos.", markers));
  CHECK(call_checks::speaksRegionalism("Mi pata me lo contó.", markers));
  CHECK(call_checks::speaksRegionalism("Llegó al toque.", markers));
  CHECK(call_checks::speaksRegionalism("¿Qué tal, causa?", markers));
  CHECK_FALSE(call_checks::speaksRegionalism("Los gatos llegaban con la pata.", markers));
  CHECK_FALSE(call_checks::speaksRegionalism("Se lastimó la pata y el brazo.", markers));
  CHECK_FALSE(call_checks::speaksRegionalism("La cámara está en línea.", markers));
  const std::vector<std::string> none;
  CHECK_FALSE(call_checks::speaksRegionalism("Los gatos llegaban con la pata.", none));
}

TEST_CASE("the clock-restraint date check names Spanish and English dates without tripping on modal verbs")
{
  CHECK(call_checks::namesADate("Hoy es miércoles."));
  CHECK(call_checks::namesADate("Miércoles por la tarde."));
  CHECK(call_checks::namesADate("Hoy es miercoles, 8 de octubre."));
  CHECK(call_checks::namesADate("Abril ha sido largo."));
  CHECK(call_checks::namesADate("Today is Wednesday."));
  CHECK(call_checks::namesADate("See you in March."));
  CHECK_FALSE(call_checks::namesADate("Is there anything I may do?"));
  CHECK_FALSE(call_checks::namesADate("We march on."));
  CHECK_FALSE(call_checks::namesADate("Mayfair is a district."));
  CHECK_FALSE(call_checks::namesADate("No date here at all."));
}

TEST_CASE("reciting a system or person block header is caught, a natural mention is not")
{
  const std::vector<std::string> none;
  CHECK(call_checks::recitesNotes("Lo que sabes de la persona (úsalo con naturalidad, no lo recites): Ana prefiere el café.", none));
  CHECK(call_checks::recitesNotes("What you know about the person (use it naturally, do not recite it): Ana likes coffee.", none));
  CHECK(call_checks::recitesNotes("Información del sistema: todo en orden.", none));
  CHECK_FALSE(call_checks::recitesNotes("Claro, Ana, te lo apunto.", none));
  CHECK_FALSE(call_checks::recitesNotes("Lo que sé de ti es que te gusta el café.", none));
  const std::vector<std::string> notes{"Agenda de hoy: libre."};
  CHECK(call_checks::recitesNotes("Agenda de hoy: libre.", notes));
}

TEST_CASE("a reply that writes an app note or invents a camera or guard state is fabricated")
{
  const auto fired = [](const std::string& reply, bool notePresent, bool cameraKnown, bool guardKnown) {
    return call_checks::fabricatedNote(
        {.reply = reply, .notePresent = notePresent, .cameraKnown = cameraKnown, .guardKnown = guardKnown});
  };
  CHECK(fired("Nota de la app: las cámaras están activas y el modo de vigilancia está configurado correctamente.", false, true, true));
  CHECK(fired("App note: the cameras are active and the guard mode is set correctly.", false, true, true));
  CHECK(fired("Nota de la app: se informó que David está bien atento a su entorno.", true, true, true));
  CHECK(fired("La cámara está en funcionamiento.", false, false, false));
  CHECK(fired("Cameras are online.", false, false, false));
  CHECK(fired("El modo de vigilancia está activo.", false, true, false));
  CHECK_FALSE(fired("La cámara de la cocina muestra actividad.", false, true, false));
  CHECK_FALSE(fired("Yes, the guard mode is active at home.", false, false, true));
  CHECK_FALSE(fired("¿Cuál es el nombre de la tarea que necesitas anotar?", false, true, true));
  CHECK_FALSE(fired("Claro, aquí tienes un chiste.", false, false, false));
}

TEST_CASE("the name-ask checks flag a missing first-turn question or a repeated one")
{
  CHECK(call_checks::asksName("¿Cómo te llamas?"));
  CHECK(call_checks::asksName("What's your name?"));
  CHECK(call_checks::asksName("Me gustaría saber tu nombre."));
  CHECK_FALSE(call_checks::asksName("Te lo apunto, David."));

  const call_checks::NameAskInput missing{.reply = "Claro, te cuento un chiste corto.", .nameUnknown = true, .firstTurn = true};
  const call_checks::NameAskInput asked{.reply = "¿Cómo te llamas?", .nameUnknown = true, .firstTurn = true};
  const call_checks::NameAskInput known{.reply = "Claro, te cuento un chiste corto.", .nameUnknown = false, .firstTurn = true};
  const call_checks::NameAskInput later{.reply = "Claro, te cuento un chiste corto.", .nameUnknown = true, .firstTurn = false};
  CHECK(call_checks::missedNameAsk(missing));
  CHECK_FALSE(call_checks::missedNameAsk(asked));
  CHECK_FALSE(call_checks::missedNameAsk(known));
  CHECK_FALSE(call_checks::missedNameAsk(later));

  const call_checks::NameAskInput again{.reply = "¿Me dices tu nombre?", .firstTurn = false};
  const call_checks::NameAskInput first{.reply = "¿Cómo te llamas?", .firstTurn = true};
  const call_checks::NameAskInput natural{.reply = "Te lo apunto, David.", .firstTurn = false};
  CHECK(call_checks::nameAskRepeated(again));
  CHECK_FALSE(call_checks::nameAskRepeated(first));
  CHECK_FALSE(call_checks::nameAskRepeated(natural));
}
