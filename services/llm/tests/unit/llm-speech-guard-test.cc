#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/turn/speech-acts.hxx>
#include <feature/llm/services/turn/speech-guard.hxx>
#include <feature/llm/services/turn/speech-render.hxx>

#include <text/iso-time.hxx>
#include <text/spoken-time.hxx>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <stdexcept>

#include "support/require-value.hxx"

namespace
{
using namespace turn::speech;

constexpr int64_t kNow = 1790000000;
constexpr int64_t kTomorrow = kNow + 86400;

GuardVerdict guarded(const Act& act, std::string lang, std::string_view reply, bool wrote = false)
{
  const Speech speech{.acts = {act}, .lang = std::move(lang), .now = kNow};
  return check({.speech = speech, .reply = reply, .wrote = wrote, .asked = true, .callsConfirmed = false});
}

AskSlot ask(std::string slot, std::vector<DatePart> dates = {}, std::vector<std::string> options = {},
            Json::Value known = Json::Value(Json::objectValue))
{
  return {.slot = std::move(slot),
          .tool = "calendar.create_event",
          .knownArgs = std::move(known),
          .reason = AskReason::Missing,
          .dates = std::move(dates),
          .options = std::move(options)};
}

Confirm confirm(std::string action, std::string_view title, std::string module = {})
{
  Json::Value args(Json::objectValue);
  if (!title.empty())
    args["title"] = std::string(title);
  return {.action = std::move(action),
          .args = std::move(args),
          .irreversible = false,
          .toolPreview = {},
          .module = std::move(module)};
}

Done done(std::string readback)
{
  return {.tool = "calendar.create_event", .fact = "Hecho.", .readback = std::move(readback)};
}
}

TEST_CASE("every verdict has a name and a feedback line of its own in both languages")
{
  const std::vector<GuardVerdict> verdicts{GuardVerdict::Pass,
                                           GuardVerdict::NotAQuestion,
                                           GuardVerdict::SlotNotAsked,
                                           GuardVerdict::SlotReasked,
                                           GuardVerdict::OptionsIncomplete,
                                           GuardVerdict::ActionUnnamed,
                                           GuardVerdict::ArgumentMissing,
                                           GuardVerdict::NotYesOrNo,
                                           GuardVerdict::ClaimedWithoutTool,
                                           GuardVerdict::IdentifierLeaked};
  std::string names;
  for (const GuardVerdict verdict : verdicts) {
    CHECK_FALSE(verdictName(verdict).empty());
    names += std::string(verdictName(verdict)) + " ";
    if (verdict != GuardVerdict::Pass) {
      CHECK_FALSE(feedback(verdict, "es").empty());
      CHECK_FALSE(feedback(verdict, "en").empty());
      CHECK(feedback(verdict, "es") != feedback(verdict, "en"));
    }
  }
  CHECK(feedback(GuardVerdict::Pass, "es").empty());
  CHECK(names.find("slot_reasked") != std::string::npos);
}

TEST_CASE("a question act is refused when the reply is not a question at all")
{
  CHECK(guarded(ask("starts_at"), "es", "Lo agendo enseguida.") == GuardVerdict::NotAQuestion);
  CHECK(guarded(ask("starts_at"), "en", "I will schedule it right away.") == GuardVerdict::NotAQuestion);
  CHECK(guarded(ask("starts_at"), "es", "¿Para cuándo lo agendo?") == GuardVerdict::Pass);
}

TEST_CASE("a slot act accepts a request, not only an interrogative, when the slot's cue is present")
{
  CHECK(guarded(ask("starts_at"), "es", "Claro, necesito que me digas la fecha y hora.") == GuardVerdict::Pass);
  CHECK(guarded(ask("starts_at"), "es", "Dime a qué hora lo agendo.") == GuardVerdict::Pass);
  CHECK(guarded(ask("starts_at"), "es", "Me dices la fecha, por favor.") == GuardVerdict::Pass);
  CHECK(guarded(ask("title"), "es", "Dime cómo se llama la tarea.") == GuardVerdict::Pass);
  CHECK(guarded(ask("starts_at"), "en", "Tell me the date and the time.") == GuardVerdict::Pass);
  CHECK(guarded(ask("starts_at"), "en", "I need the date and time.") == GuardVerdict::Pass);
  CHECK(guarded(ask("starts_at"), "en", "Let me know what time works.") == GuardVerdict::Pass);

  CHECK(guarded(ask("starts_at"), "es", "Lo agendo enseguida.") == GuardVerdict::NotAQuestion);
  CHECK(guarded(ask("starts_at"), "es", "La fecha y hora es lo que falta.") == GuardVerdict::NotAQuestion);
  CHECK(guarded(ask("starts_at"), "es", "Dime algo.") == GuardVerdict::SlotNotAsked);
  CHECK(guarded(ask("starts_at"), "en", "Tell me something.") == GuardVerdict::SlotNotAsked);
  CHECK(guarded(ask("starts_at"), "en", "I need your help.") == GuardVerdict::SlotNotAsked);
}

TEST_CASE("a question act is refused when the reply asks nothing, and when it asks something it already knew")
{
  CHECK(guarded(ask("title"), "es", "¿Dónde lo guardo?") == GuardVerdict::SlotNotAsked);
  CHECK(guarded(ask("starts_at"), "en", "Which camera do you mean?") == GuardVerdict::SlotNotAsked);
  Json::Value known(Json::objectValue);
  known["starts_at"] = "2026-10-08T17:00:00+00:00";
  CHECK(guarded(ask("subject", {}, {}, known), "es", "¿De quién es? ¿A qué hora?") == GuardVerdict::SlotReasked);
  CHECK(guarded(ask("subject", {}, {}, known), "es", "¿De quién es?") == GuardVerdict::Pass);
}

TEST_CASE("a question act that carries dates must name one of them")
{
  const std::vector<DatePart> dates{{.kind = DateKind::Clock, .epoch = kTomorrow}};
  const std::string es = dateSurface(dates.front(), "es", kNow);
  const std::string en = dateSurface(dates.front(), "en", kNow);
  CHECK(guarded(ask("starts_at", dates), "es", "¿A qué hora lo agendo, " + es + "?") == GuardVerdict::Pass);
  CHECK(guarded(ask("starts_at", dates), "es", "¿A qué hora lo agendo?") == GuardVerdict::SlotNotAsked);
  CHECK(guarded(ask("starts_at", dates), "en", "What time shall I schedule it, " + en + "?") == GuardVerdict::Pass);
  CHECK(guarded(ask("starts_at", dates), "en", "What time shall I schedule it?") == GuardVerdict::SlotNotAsked);
}

TEST_CASE("a question act that carries options must name them all, or the first three and another")
{
  const std::vector<std::string> three{"Casa", "Trabajo", "Viaje"};
  CHECK(guarded(ask("project", {}, three), "es", "¿En cuál proyecto va: Casa, Trabajo, Viaje?") == GuardVerdict::Pass);
  CHECK(guarded(ask("project", {}, three), "es", "¿En cuál proyecto va: Casa, Trabajo?") == GuardVerdict::OptionsIncomplete);
  const std::vector<std::string> five{"Casa", "Trabajo", "Viaje", "Gimnasio", "Colegio"};
  CHECK(guarded(ask("project", {}, five), "es", "¿En cuál proyecto va: Casa, Trabajo, Viaje u otro?") ==
        GuardVerdict::Pass);
  CHECK(guarded(ask("project", {}, five), "es", "¿En cuál proyecto va: Casa, Trabajo, Viaje?") ==
        GuardVerdict::OptionsIncomplete);
  CHECK(guarded(ask("project", {}, five), "en", "Which project: Casa, Trabajo, Viaje or another?") == GuardVerdict::Pass);
}

TEST_CASE("a confirmation must name the action, every surface it carries and ask a yes-or-no question")
{
  CHECK(guarded(confirm("calendar.create_event", "Cena con Marta"), "es",
                "¿Quieres que agende «Cena con Marta»?") == GuardVerdict::Pass);
  CHECK(guarded(confirm("calendar.create_event", "Cena con Marta"), "es",
                "¿Quieres que lo haga?") == GuardVerdict::ActionUnnamed);
  CHECK(guarded(confirm("calendar.create_event", "Cena con Marta"), "es",
                "¿Agendo la cena?") == GuardVerdict::ArgumentMissing);
  CHECK(guarded(confirm("modules.enable", {}, "Productividad"), "es",
                "¿Activo el módulo Productividad?") == GuardVerdict::Pass);
  CHECK(guarded(confirm("modules.enable", {}, "Productividad"), "es",
                "¿Activo el módulo?") == GuardVerdict::ArgumentMissing);
  CHECK(guarded(confirm("calendar.create_event", "Cena con Marta"), "en",
                "Shall I schedule “Cena con Marta”?") == GuardVerdict::Pass);
  CHECK(guarded(confirm("calendar.create_event", "Cena con Marta"), "es",
                "Agendo «Cena con Marta».") == GuardVerdict::NotYesOrNo);
}

TEST_CASE("a confirmation speaks the date it carries")
{
  Json::Value args(Json::objectValue);
  args["title"] = "Cena";
  args["starts_at"] = "2026-10-08T17:00:00+00:00";
  const Act act = Confirm{.action = "calendar.create_event",
                          .args = std::move(args),
                          .irreversible = true,
                          .toolPreview = "Esto cancelaría la cena.",
                          .module = {}};
  const auto at = iso_time::parse("2026-10-08T17:00:00+00:00");
  REQUIRE(at.has_value());
  const spoken_time::When when{.epoch = requireValue(at), .now = kNow, .lang = "es", .day = spoken_time::Day::Relative};
  CHECK(guarded(act, "es", "¿Agendo «Cena»?") == GuardVerdict::ArgumentMissing);
  CHECK(guarded(act, "es", "¿Agendo «Cena» " + spoken_time::day(when) + "?") == GuardVerdict::Pass);
  CHECK(guarded(act, "es", "¿Agendo «Cena» " + spoken_time::clock(when) + "?") == GuardVerdict::Pass);
}

TEST_CASE("a choice must name each of its two actions")
{
  const Act act = Choose{.options = {"task.create", "memory.remind"}};
  CHECK(guarded(act, "es", "¿Quieres que lo anote como tarea o que te lo recuerde?") == GuardVerdict::Pass);
  CHECK(guarded(act, "es", "¿Lo anoto?") == GuardVerdict::OptionsIncomplete);
  CHECK(guarded(act, "en", "Should I add it as a task or remind you?") == GuardVerdict::Pass);
}

TEST_CASE("a choice matches the options' display labels")
{
  const Act act = Choose{.options = {"calendar.create_event", "task.create"}};
  CHECK(guarded(act, "es", "¿Quieres que cree un evento en el calendario o una tarea?") == GuardVerdict::Pass);
  CHECK(guarded(act, "es", "¿Quieres que organice un evento en el calendario o anote una tarea?") == GuardVerdict::Pass);
  CHECK(guarded(act, "en", "Should I add an event on the calendar or a task?") == GuardVerdict::Pass);
  CHECK(guarded(act, "es", "¿Quieres que lo haga?") == GuardVerdict::OptionsIncomplete);
  CHECK(guarded(act, "en", "Should I do something?") == GuardVerdict::OptionsIncomplete);
}

TEST_CASE("a confirmation accepts a yes-or-no request and refuses a bare statement")
{
  CHECK(guarded(confirm("calendar.create_event", "Cena con Marta"), "es",
                "¿Quieres que agende «Cena con Marta»?") == GuardVerdict::Pass);
  CHECK(guarded(confirm("calendar.create_event", "Cena con Marta"), "es",
                "Por favor, dime si quieres que agende «Cena con Marta».") == GuardVerdict::Pass);
  CHECK(guarded(confirm("calendar.cancel_event", "Dentist appointment"), "en",
                "Do you want me to cancel the dentist appointment?") == GuardVerdict::Pass);
  CHECK(guarded(confirm("calendar.cancel_event", "Dentist appointment"), "en",
                "Please let me know if you want me to cancel the dentist appointment.") == GuardVerdict::Pass);
  CHECK(guarded(confirm("calendar.cancel_event", "Dentist appointment"), "en",
                "Let me confirm the appointment for you.") == GuardVerdict::NotYesOrNo);
  CHECK(guarded(confirm("calendar.create_event", "Cena con Marta"), "es",
                "Agendo «Cena con Marta».") == GuardVerdict::NotYesOrNo);
}

TEST_CASE("a done act must speak back what the tool read back, and a read without one asks for nothing")
{
  CHECK(guarded(done("el jueves 8 a las 5 de la tarde"), "es", "Listo, quedó para el jueves 8 a las 5 de la tarde.",
                true) == GuardVerdict::Pass);
  CHECK(guarded(done("el jueves 8 a las 5 de la tarde"), "es", "Listo, ya quedó agendada tu reunión.", true) ==
        GuardVerdict::ArgumentMissing);
  CHECK(guarded(done({}), "es", "Listo, ya quedó agendada tu reunión.", true) == GuardVerdict::Pass);
  CHECK(guarded(Done{.tool = "task.list", .fact = "Tienes dos tareas.", .readback = {}}, "en", "You have two tasks.",
                true) == GuardVerdict::Pass);
}

TEST_CASE("an offer names the module it is about and asks")
{
  const Act act = Offer{.module = "productivity", .name = "Productividad", .facts = "está apagado", .pendingIntent = {}};
  CHECK(guarded(act, "es", "El módulo Productividad está apagado. ¿Lo activo?") == GuardVerdict::Pass);
  CHECK(guarded(act, "es", "El módulo Productividad está apagado.") == GuardVerdict::SlotNotAsked);
  CHECK(guarded(act, "es", "¿Lo activo?") == GuardVerdict::SlotNotAsked);
}

TEST_CASE("a refusal is kept and a claim of work that never ran is not, on every act")
{
  CHECK(guarded(Refused{.tool = "task.create", .reason = "project_create_unavailable"}, "es",
                "No puedo crear proyectos, así que la tarea no se anotó.") == GuardVerdict::Pass);
  CHECK(guarded(Refused{.tool = "task.create", .reason = "project_create_unavailable"}, "es",
                "Anoté la tarea en un proyecto nuevo.") == GuardVerdict::ClaimedWithoutTool);
  CHECK(guarded(Declined{}, "es", "Vale, no lo hago.") == GuardVerdict::Pass);
  CHECK(guarded(Declined{}, "es", "Ya lo agendé.") == GuardVerdict::ClaimedWithoutTool);
  CHECK(guarded(Misunderstood{}, "en", "I did not catch that. Say it again.") == GuardVerdict::Pass);
  CHECK(guarded(Unactionable{.reason = "no_matching_action"}, "es", "Eso no lo puedo hacer.") == GuardVerdict::Pass);
  CHECK(guarded(Offer{.module = "productivity", .name = "Productividad", .facts = {}, .pendingIntent = {}}, "es",
                "¿Activo el módulo Productividad? Ya lo activé.") == GuardVerdict::ClaimedWithoutTool);
}

TEST_CASE("the release window opens through the first question or two sentences, whichever comes first")
{
  CHECK(releaseWindowOf("¿Qué día? Puedo agendarlo cuando quieras.").text == "¿Qué día?");
  CHECK(releaseWindowOf("¿Qué día? Puedo agendarlo cuando quieras.").settled);
  CHECK(releaseWindowOf("Listo.").text == "Listo.");
  CHECK_FALSE(releaseWindowOf("Listo.").settled);
  CHECK(releaseWindowOf("Muy bien... sigo.").text == "Muy bien... sigo.");
  CHECK(releaseWindowOf("Muy bien... sigo.").settled);
  CHECK(releaseWindowOf("Uno. Dos. ¿Qué día?").text == "Uno. Dos.");
  CHECK(releaseWindowOf("Uno. Dos. ¿Qué día?").settled);
  CHECK(releaseWindowOf("¿Qué día y a qué hora").text == "¿Qué día y a qué hora");
  CHECK_FALSE(releaseWindowOf("¿Qué día y a qué hora").settled);
  CHECK(releaseWindowOf("").text.empty());
  CHECK_FALSE(releaseWindowOf("").settled);
  CHECK(releaseWindowOf("Recuerda, necesito el nombre.\n¿Para qué día?").text ==
        "Recuerda, necesito el nombre.\n¿Para qué día?");
  CHECK(releaseWindowOf("Recuerda, necesito el nombre.\n¿Para qué día?").settled);
  CHECK(releaseWindowOf("Recuerda, necesito el nombre.  \n¿Para qué día?").text ==
        "Recuerda, necesito el nombre.  \n¿Para qué día?");
  CHECK(releaseWindowOf("Recuerda, necesito el nombre.  \n¿Para qué día?").settled);
  CHECK(releaseWindowOf("Recuerda, necesito el nombre!\n¿Para qué día?").text ==
        "Recuerda, necesito el nombre!\n¿Para qué día?");
  CHECK(releaseWindowOf("Recuerda, necesito el nombre!\n¿Para qué día?").settled);
  CHECK(releaseWindowOf("Listo.  \nLo agendo enseguida.").text == "Listo.  \nLo agendo enseguida.");
  CHECK(releaseWindowOf("Uno. Dos.\n¿Qué día?").text == "Uno. Dos.");
  CHECK(releaseWindowOf("Uno. Dos.\n¿Qué día?").settled);
}

TEST_CASE("a question act is released at its first question, capped at two sentences")
{
  const Speech speech{.acts = {ask("starts_at")}, .lang = "es", .now = kNow};
  CHECK(check({.speech = speech,
               .reply = "Entendido, necesito que me indiques la fecha y hora. ¿Para cuándo quieres que te lo recuerde?",
               .asked = true,
               .sentenceOnly = true}) == GuardVerdict::Pass);
  CHECK(check({.speech = speech,
               .reply = "Entendido, necesito que me indiques la fecha y hora.\n¿Para cuándo quieres que te lo recuerde?",
               .asked = true,
               .sentenceOnly = true}) == GuardVerdict::Pass);
  CHECK(check({.speech = speech,
               .reply = "Entendido, necesito que me indiques la fecha y hora.  \n¿Para cuándo quieres que te lo recuerde?",
               .asked = true,
               .sentenceOnly = true}) == GuardVerdict::Pass);
  CHECK(check({.speech = speech,
               .reply = "Entendido, necesito que me indiques la fecha y hora!\n¿Para cuándo quieres que te lo recuerde?",
               .asked = true,
               .sentenceOnly = true}) == GuardVerdict::Pass);
  CHECK(check({.speech = speech,
               .reply = "Entendido, necesito la fecha.\nLo agendo enseguida.",
               .asked = true,
               .sentenceOnly = true}) == GuardVerdict::NotAQuestion);
  CHECK(check({.speech = speech,
               .reply = "Claro.\nDe acuerdo.\n¿Para cuándo lo agendo?",
               .asked = true,
               .sentenceOnly = true}) == GuardVerdict::NotAQuestion);
  CHECK(check({.speech = speech, .reply = "Lo haré enseguida. Eso es todo.", .asked = true, .sentenceOnly = true}) ==
        GuardVerdict::NotAQuestion);
  CHECK(check({.speech = speech, .reply = "Claro. De acuerdo. ¿Para cuándo lo agendo?", .asked = true, .sentenceOnly = true}) ==
        GuardVerdict::NotAQuestion);
  CHECK(check({.speech = speech, .reply = "Lo haré. ¿Para cuándo lo agendo?", .asked = true, .sentenceOnly = true}) ==
        GuardVerdict::Pass);
  CHECK(check({.speech = speech, .reply = "¿Para cuándo lo agendo? Cuando quieras.", .asked = true, .sentenceOnly = true}) ==
        GuardVerdict::Pass);
  CHECK(check({.speech = speech, .reply = "¿Lo agendo? Solo dime algo.", .asked = true}) == GuardVerdict::SlotNotAsked);
}

TEST_CASE("a slot act released at its first question still names every option it carries")
{
  const std::vector<std::string> three{"Casa", "Trabajo", "Viaje"};
  const Speech speech{.acts = {ask("project", {}, three)}, .lang = "es", .now = kNow};
  CHECK(check({.speech = speech,
               .reply = "Entendido. ¿En cuál proyecto va: Casa, Trabajo, Viaje?",
               .asked = true,
               .sentenceOnly = true}) == GuardVerdict::Pass);
  CHECK(check({.speech = speech,
               .reply = "Entendido. ¿En cuál proyecto va: Casa, Trabajo?",
               .asked = true,
               .sentenceOnly = true}) == GuardVerdict::OptionsIncomplete);
}

TEST_CASE("a confirmation released at its first question keeps refusing a reply that never asks yes or no")
{
  const Speech speech{.acts = {confirm("calendar.create_event", "Cena con Marta")}, .lang = "es", .now = kNow};
  CHECK(check({.speech = speech,
               .reply = "Voy a agendar «Cena con Marta». ¿Quieres que lo agende?",
               .asked = true,
               .sentenceOnly = true}) == GuardVerdict::Pass);
  CHECK(check({.speech = speech,
               .reply = "Voy a agendar «Cena con Marta». Lo agendo ya.",
               .asked = true,
               .sentenceOnly = true}) == GuardVerdict::NotYesOrNo);
}

TEST_CASE("a leaked identifier is refused on any act")
{
  const Speech askSpeech{.acts = {ask("starts_at")}, .lang = "es", .now = kNow};
  CHECK(check({.speech = askSpeech, .reply = "Dime el starts_at, por favor.", .asked = true}) ==
        GuardVerdict::IdentifierLeaked);
  CHECK(check({.speech = askSpeech, .reply = "¿Para cuándo lo agendo?", .asked = true}) == GuardVerdict::Pass);
  const Speech cancelSpeech{.acts = {Confirm{.action = "calendar.cancel_event",
                                             .args = Json::Value(Json::objectValue),
                                             .irreversible = true,
                                             .toolPreview = {},
                                             .module = {}}},
                            .lang = "es",
                            .now = kNow};
  CHECK(check({.speech = cancelSpeech, .reply = "¿Cancelo la cita con calendar.cancel_event?", .asked = true}) ==
        GuardVerdict::IdentifierLeaked);
}

TEST_CASE("hard failures are fatal and soft ones are not")
{
  const Speech askSpeech{.acts = {ask("starts_at")}, .lang = "es", .now = kNow};
  CHECK(hardFailure(GuardVerdict::ClaimedWithoutTool, askSpeech));
  CHECK(hardFailure(GuardVerdict::IdentifierLeaked, askSpeech));
  CHECK_FALSE(hardFailure(GuardVerdict::SlotNotAsked, askSpeech));
  CHECK_FALSE(hardFailure(GuardVerdict::SlotReasked, askSpeech));
  CHECK_FALSE(hardFailure(GuardVerdict::NotAQuestion, askSpeech));
  CHECK_FALSE(hardFailure(GuardVerdict::ArgumentMissing, askSpeech));

  Json::Value args(Json::objectValue);
  args["title"] = "Cita";
  const Speech hardConfirm{.acts = {Confirm{.action = "calendar.cancel_event",
                                             .args = args,
                                             .irreversible = true,
                                             .toolPreview = {},
                                             .module = {}}},
                            .lang = "es",
                            .now = kNow};
  CHECK(hardFailure(GuardVerdict::NotYesOrNo, hardConfirm));
  const Speech softConfirm{.acts = {Confirm{.action = "calendar.create_event",
                                            .args = args,
                                            .irreversible = false,
                                            .toolPreview = {},
                                            .module = {}}},
                           .lang = "es",
                           .now = kNow};
  CHECK_FALSE(hardFailure(GuardVerdict::NotYesOrNo, softConfirm));

  const Speech choose{.acts = {Choose{.options = {"task.create", "memory.remind"}}}, .lang = "es", .now = kNow};
  CHECK(hardFailure(GuardVerdict::OptionsIncomplete, choose));
  const Speech askOptions{.acts = {ask("project", {}, {"Casa", "Trabajo"})}, .lang = "es", .now = kNow};
  CHECK_FALSE(hardFailure(GuardVerdict::OptionsIncomplete, askOptions));
}
